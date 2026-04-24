#include "pch.h"
#include "imgui.h"
#include "ProcessesView.h"
#include <algorithm>
#include "SortHelper.h"
#include "Processes.h"
#include "TabManager.h"
#include <shellapi.h>
#include "FormatHelper.h"
#include "ImGuiExt.h"
#include "Globals.h"
#include "colors.h"
#include <WICTextureLoader.h>
#include <wincodec.h>
#include "resource.h"
#include "LoggerView.h"
#include "RemoteClient.h"

using namespace ImGui;

namespace {
	struct AddressMapStats {
		ULONG64 Lowest{ 0 };
		ULONG64 Highest{ 0 };
		ULONG64 CommittedBytes{ 0 };
		size_t RegionCount{ 0 };
	};

	AddressMapStats ComputeAddressMapStats(const std::vector<std::shared_ptr<WinSys::MemoryRegionItem>>& regions) {
		AddressMapStats stats;
		if (regions.empty())
			return stats;
		stats.Lowest = reinterpret_cast<ULONG64>(regions.front()->BaseAddress);
		for (auto const& region : regions) {
			auto base = reinterpret_cast<ULONG64>(region->BaseAddress);
			auto end = base + static_cast<ULONG64>(region->RegionSize);
			if (stats.Highest < end)
				stats.Highest = end;
			if (region->State == MEM_COMMIT)
				stats.CommittedBytes += static_cast<ULONG64>(region->RegionSize);
			stats.RegionCount++;
		}
		return stats;
	}

	const char* StateToString(DWORD state) {
		switch (state) {
		case MEM_COMMIT: return "Commit";
		case MEM_RESERVE: return "Reserve";
		case MEM_FREE: return "Free";
		default: return "Unknown";
		}
	}

	const char* TypeToString(DWORD type) {
		switch (type) {
		case MEM_IMAGE: return "Image";
		case MEM_MAPPED: return "Mapped";
		case MEM_PRIVATE: return "Private";
		case 0: return "-";
		default: return "Other";
		}
	}

	std::string ProtectToString(DWORD protect) {
		if (protect == 0)
			return "-";

		std::string suffix;
		if ((protect & PAGE_GUARD) == PAGE_GUARD)
			suffix += "|Guard";
		if ((protect & PAGE_NOCACHE) == PAGE_NOCACHE)
			suffix += "|NoCache";
		if ((protect & PAGE_WRITECOMBINE) == PAGE_WRITECOMBINE)
			suffix += "|WriteCombine";

		protect &= ~(PAGE_GUARD | PAGE_NOCACHE | PAGE_WRITECOMBINE);
		const char* base = "Unknown";
		switch (protect) {
		case PAGE_NOACCESS: base = "NoAccess"; break;
		case PAGE_READONLY: base = "R"; break;
		case PAGE_READWRITE: base = "RW"; break;
		case PAGE_WRITECOPY: base = "WC"; break;
		case PAGE_EXECUTE: base = "X"; break;
		case PAGE_EXECUTE_READ: base = "RX"; break;
		case PAGE_EXECUTE_READWRITE: base = "RWX"; break;
		case PAGE_EXECUTE_WRITECOPY: base = "XWC"; break;
		}
		return std::string(base) + suffix;
	}

	ImVec4 BlendColor(const ImVec4& a, const ImVec4& b, float t, float alpha = 1.0f) {
		return ImVec4(
			a.x + (b.x - a.x) * t,
			a.y + (b.y - a.y) * t,
			a.z + (b.z - a.z) * t,
			alpha);
	}

	ImU32 GetProcessRegionColor(const WinSys::MemoryRegionItem& region) {
		auto bg = GetStyleColorVec4(ImGuiCol_FrameBg);
		auto text = GetStyleColorVec4(ImGuiCol_Text);
		auto accent = GetStyleColorVec4(ImGuiCol_PlotHistogram);
		auto accent2 = GetStyleColorVec4(ImGuiCol_PlotHistogramHovered);
		auto selected = GetStyleColorVec4(ImGuiCol_HeaderHovered);
		auto luminance = bg.x * 0.2126f + bg.y * 0.7152f + bg.z * 0.0722f;
		float alpha = luminance > 0.55f ? 0.55f : 0.88f;

		if (region.State == MEM_FREE)
			return GetColorU32(BlendColor(bg, text, luminance > 0.55f ? 0.10f : 0.18f, alpha));
		if (region.State == MEM_RESERVE)
			return GetColorU32(BlendColor(bg, text, luminance > 0.55f ? 0.20f : 0.28f, alpha));
		if ((region.Protect & PAGE_GUARD) == PAGE_GUARD)
			return GetColorU32(BlendColor(accent, accent2, 0.55f, alpha));
		if ((region.Protect & PAGE_EXECUTE_READWRITE) == PAGE_EXECUTE_READWRITE)
			return GetColorU32(BlendColor(selected, ImVec4(1.0f, 0.35f, 0.25f, 1.0f), 0.65f, alpha));
		if ((region.Protect & PAGE_EXECUTE_READ) == PAGE_EXECUTE_READ)
			return GetColorU32(BlendColor(accent2, ImVec4(1.0f, 0.55f, 0.20f, 1.0f), 0.50f, alpha));
		if ((region.Type & MEM_IMAGE) == MEM_IMAGE)
			return GetColorU32(BlendColor(accent, selected, 0.35f, alpha));
		if ((region.Type & MEM_MAPPED) == MEM_MAPPED)
			return GetColorU32(BlendColor(selected, text, 0.22f, alpha));
		if ((region.Type & MEM_PRIVATE) == MEM_PRIVATE)
			return GetColorU32(BlendColor(accent, ImVec4(0.20f, 0.85f, 0.45f, 1.0f), 0.45f, alpha));
		return GetColorU32(BlendColor(bg, selected, 0.45f, alpha));
	}

	const WinSys::MemoryRegionItem* DrawProcessAddressMap(const char* id, const std::vector<std::shared_ptr<WinSys::MemoryRegionItem>>& regions, ImVec2 size) {
		if (regions.empty()) {
			ImGui::TextDisabled("No memory regions available");
			return nullptr;
		}

		ImGui::InvisibleButton(id, size);
		auto min = ImGui::GetItemRectMin();
		auto max = ImGui::GetItemRectMax();
		auto* draw = ImGui::GetWindowDrawList();
		draw->AddRectFilled(min, max, IM_COL32(17, 22, 28, 255), 4.0f);
		draw->AddRect(min, max, IM_COL32(55, 70, 78, 255), 4.0f);

		auto lowest = reinterpret_cast<ULONG64>(regions.front()->BaseAddress);
		ULONG64 highest = lowest;
		for (auto const& region : regions) {
			auto end = reinterpret_cast<ULONG64>(region->BaseAddress) + static_cast<ULONG64>(region->RegionSize);
			if (highest < end)
				highest = end;
		}
		auto totalSpan = highest > lowest ? highest - lowest : 1ull;

		float padding = 8.0f;
		float mapWidth = size.x - padding * 2.0f;
		float mapHeight = size.y - padding * 2.0f;
		const int rows = 6;
		float rowGap = 4.0f;
		float rowHeight = (mapHeight - rowGap * static_cast<float>(rows - 1)) / static_cast<float>(rows);
		rowHeight = (std::max)(8.0f, rowHeight);

		for (int row = 0; row < rows; row++) {
			float y = min.y + padding + row * (rowHeight + rowGap);
			draw->AddRectFilled(ImVec2(min.x + padding, y), ImVec2(min.x + padding + mapWidth, y + rowHeight),
				GetColorU32(BlendColor(GetStyleColorVec4(ImGuiCol_FrameBg), GetStyleColorVec4(ImGuiCol_Border), 0.15f, 0.75f)), 2.0f);
		}

		const WinSys::MemoryRegionItem* hovered = nullptr;
		auto mouse = ImGui::GetIO().MousePos;
		bool mapHovered = ImGui::IsItemHovered();

		for (int index = 0; index < static_cast<int>(regions.size()); index++) {
			auto const& region = regions[index];
			auto base = reinterpret_cast<ULONG64>(region->BaseAddress);
			auto end = base + static_cast<ULONG64>(region->RegionSize);
			auto color = GetProcessRegionColor(*region);
			float startRatio = static_cast<float>(base - lowest) / static_cast<float>(totalSpan);
			float endRatio = static_cast<float>(end - lowest) / static_cast<float>(totalSpan);
			float x1 = min.x + padding + startRatio * mapWidth;
			float x2 = min.x + padding + endRatio * mapWidth;
			x2 = (std::max)(x2, x1 + 3.0f);
			int row = index % rows;
			float y = min.y + padding + row * (rowHeight + rowGap);
			draw->AddRectFilled(ImVec2(x1, y), ImVec2(x2, y + rowHeight), color, 2.0f);

			if (mapHovered && mouse.x >= x1 && mouse.x <= x2 && mouse.y >= y && mouse.y <= y + rowHeight)
				hovered = region.get();
		}

		return hovered;
	}
}
extern ID3D11Device* g_pd3dDevice;
extern ID3D11DeviceContext* g_pd3dDeviceContext;

namespace {
	std::wstring Utf8ToWide(const char* utf8) {
		if (!utf8 || !utf8[0]) return {};
		int size = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
		std::wstring result(size - 1, 0);
		MultiByteToWideChar(CP_UTF8, 0, utf8, -1, result.data(), size);
		return result;
	}

	std::shared_ptr<WinSys::ProcessInfo> ProcessInfoFromNet(const ProcessInfoNet& net) {
		auto pi = std::make_shared<WinSys::ProcessInfo>();
		pi->Id = net.Id;
		pi->ParentId = net.ParentId;
		pi->SessionId = net.SessionId;
		pi->HandleCount = net.HandleCount;
		pi->ThreadCount = net.ThreadCount;
		pi->PeakThreads = net.PeakThreads;
		pi->CreateTime = net.CreateTime;
		pi->UserTime = net.UserTime;
		pi->KernelTime = net.KernelTime;
		pi->WorkingSetPrivateSize = net.WorkingSetPrivateSize;
		pi->VirtualSize = (size_t)net.VirtualSize;
		pi->PeakVirtualSize = (size_t)net.PeakVirtualSize;
		pi->WorkingSetSize = (size_t)net.WorkingSetSize;
		pi->PeakWorkingSetSize = (size_t)net.PeakWorkingSetSize;
		pi->PrivatePageCount = (size_t)net.PrivatePageCount;
		pi->PagedPoolUsage = (size_t)net.PagedPoolUsage;
		pi->PeakPagedPoolUsage = (size_t)net.PeakPagedPoolUsage;
		pi->NonPagedPoolUsage = (size_t)net.NonPagedPoolUsage;
		pi->PeakNonPagedPoolUsage = (size_t)net.PeakNonPagedPoolUsage;
		pi->PagefileUsage = (size_t)net.PagefileUsage;
		pi->PeakPagefileUsage = (size_t)net.PeakPagefileUsage;
		pi->CPU = net.CPU;
		pi->BasePriority = net.BasePriority;
		pi->PageFaultCount = net.PageFaultCount;
		pi->HardFaultCount = net.HardFaultCount;
		pi->Key = WinSys::ProcessOrThreadKey{ net.CreateTime, net.Id };
		pi->SetProcessName(Utf8ToWide(net.ImageName));
		pi->SetNativeImagePath(Utf8ToWide(net.ImagePath));
		return pi;
	}
}

ProcessesView::ProcessesView() : ViewBase(5000) { }

void ProcessesView::RefreshNow() {
	if (RemoteClient::IsConnected()) {
		// Launch async fetch if not already pending
		if (!_remoteFetchPending) {
			_remoteFuture = std::async(std::launch::async, []() {
				return RemoteClient::GetProcesses();
			});
			_remoteFetchPending = true;
		}
		// Check if async result is ready (non-blocking)
		if (_remoteFetchPending && _remoteFuture.valid() &&
			_remoteFuture.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
			auto netProcs = _remoteFuture.get();
			_remoteFetchPending = false;
			_processes.clear();
			_processes.reserve(netProcs.size());
			_processesEx.clear();
			for (auto& np : netProcs)
				_processes.push_back(ProcessInfoFromNet(np));
			if (_specs)
				DoSort(_specs->ColumnIndex, _specs->SortDirection == ImGuiSortDirection_Ascending);
			LoggerView::AddLog(LoggerView::UserModeLog, "Enumerated %zu processes (remote)", _processes.size());
			MarkUpdated();
		}
		return;
	}

	auto empty = _processes.empty();
	if (empty) {
		_processes.reserve(1024);
		_processesEx.reserve(1024);
	}
	_pm.EnumProcesses();
	if (empty)
		_processes = _pm.GetProcesses();
	else
		DoUpdate();
	if (_specs)
		DoSort(_specs->ColumnIndex, _specs->SortDirection == ImGuiSortDirection_Ascending);
	MarkUpdated();
}

void ProcessesView::BuildWindow() {
	BuildToolBar();
	BuildTable();
	std::vector<WinSys::ProcessOrThreadKey> keys;
	for (const auto& [key, p] : _processProperties) {
		if (p->WindowOpen)
			BuildPropertiesWindow(p.get());
		else
			keys.push_back(p->GetProcess()->Key);
	}

	for (auto& key : keys)
		_processProperties.erase(key);

	keys.clear();
	for (const auto& [key, window] : _processSecurityWindows) {
		if (window->WindowOpen)
			BuildSecurityWindow(window.get());
		else if (!window->View.HasPendingAsync())
			keys.push_back(key);
	}

	for (auto& key : keys)
		_processSecurityWindows.erase(key);
}

void ProcessesView::DoSort(int col, bool asc) {
	const bool remote = RemoteClient::IsConnected();
	std::sort(_processes.begin(), _processes.end(), [=](const auto& p1, const auto& p2) {
		switch (col) {
		case 0: return SortHelper::SortStrings(p1->GetImageName(), p2->GetImageName(), asc);
		case 1: return SortHelper::SortNumbers(p1->Id, p2->Id, asc);
		case 2: return SortHelper::SortStrings(remote ? std::wstring() : GetProcessInfoEx(p1.get()).UserName(), remote ? std::wstring() : GetProcessInfoEx(p2.get()).UserName(), asc);
		case 3: return SortHelper::SortNumbers(p1->SessionId, p2->SessionId, asc);
		case 4: return SortHelper::SortNumbers(p1->CPU, p2->CPU, asc);
		case 5: return SortHelper::SortNumbers(p1->ParentId, p2->ParentId, asc);
		case 6: return SortHelper::SortNumbers(p1->CreateTime, p2->CreateTime, asc);
		case 7: return SortHelper::SortNumbers(p1->PrivatePageCount, p2->PrivatePageCount, asc);
		case 8: return SortHelper::SortNumbers(p1->BasePriority, p2->BasePriority, asc);
		case 9: return SortHelper::SortNumbers(p1->ThreadCount, p2->ThreadCount, asc);
		case 10: return SortHelper::SortNumbers(p1->HandleCount, p2->HandleCount, asc);
		case 11: return SortHelper::SortNumbers(p1->WorkingSetSize, p2->WorkingSetSize, asc);
		case 12: return SortHelper::SortStrings(remote ? p1->GetNativeImagePath() : GetProcessInfoEx(p1.get()).GetExecutablePath(), remote ? p2->GetNativeImagePath() : GetProcessInfoEx(p2.get()).GetExecutablePath(), asc);
		case 13: return SortHelper::SortNumbers(p1->KernelTime + p1->UserTime, p2->KernelTime + p2->UserTime, asc);
		case 14: return SortHelper::SortNumbers(p1->PeakThreads, p2->PeakThreads, asc);
		case 15: return SortHelper::SortNumbers(p1->VirtualSize, p2->VirtualSize, asc);
		case 16: return SortHelper::SortNumbers(p1->PeakWorkingSetSize, p2->PeakWorkingSetSize, asc);
		case 17: return SortHelper::SortNumbers(
			remote ? 0 : static_cast<int>(GetProcessInfoEx(p1.get()).GetAttributes(_pm)),
			remote ? 0 : static_cast<int>(GetProcessInfoEx(p2.get()).GetAttributes(_pm)),
			asc);
		case 18: return SortHelper::SortNumbers(p1->PagedPoolUsage, p2->PagedPoolUsage, asc);

		}
	return false;
		});
}

ProcessInfoEx& ProcessesView::GetProcessInfoEx(WinSys::ProcessInfo* pi) const {
	auto it = _processesEx.find(pi->Key);
	if (it != _processesEx.end())
		return it->second;

	ProcessInfoEx px(pi);
	_processesEx.insert({ pi->Key, std::move(px) });
	return GetProcessInfoEx(pi);
}

void ProcessesView::DoUpdate() {
	for (auto& pi : _pm.GetNewProcesses()) {
		_processes.push_back(pi);
		auto& px = GetProcessInfoEx(pi.get());
		px.New(2000);
	}

	for (auto& pi : _pm.GetTerminatedProcesses()) {
		auto& px = GetProcessInfoEx(pi.get());
		px.Term(2000);
	}
}

bool ProcessesView::KillProcess(uint32_t id) {
	auto process = WinSys::Process::OpenById(id, WinSys::ProcessAccessMask::Terminate);
	if (process == nullptr)
		return false;

	return process->Terminate();
}

bool ProcessesView::TryKillProcess(WinSys::ProcessInfo* pi, bool& success) {
	_modalOpen = true;
	CStringA text;
	text.Format("Kill process %u (%ws)?",
		_selectedProcess->Id, _selectedProcess->GetImageName().c_str());

	auto result = SimpleMessageBox::ShowModal("Kill Process?", text, MessageBoxButtons::OkCancel);
	if (result != MessageBoxResult::StillOpen) {
		_modalOpen = false;
		if (result == MessageBoxResult::OK) {
			success = KillProcess(_selectedProcess->Id);
			if (success)
				_selectedProcess.reset();
		}
		return true;
	}
	return false;
}

void ProcessesView::BuildTable() {
	auto& g = Globals::Get();
	const bool remote = RemoteClient::IsConnected();

	//(ImVec2(size.x, size.y / 2));
	if (BeginTable("procTable", 12, ImGuiTableFlags_BordersV * 0 | ImGuiTableFlags_Sortable |
		ImGuiTableFlags_Resizable | ImGuiTableFlags_Reorderable | 0 * ImGuiTableFlags_NoSavedSettings | ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY |
		ImGuiTableFlags_RowBg | ImGuiTableFlags_Hideable | ImGuiTableFlags_NoSavedSettings)) {
		TableSetupScrollFreeze(2, 1);
		TableSetupColumn("Name", ImGuiTableColumnFlags_NoHide | ImGuiTableColumnFlags_NoReorder);
		TableSetupColumn("Id", ImGuiTableColumnFlags_NoHide | ImGuiTableColumnFlags_NoReorder);
		TableSetupColumn("User");
		TableSetupColumn("Session");
		TableSetupColumn("CPU (%)");
		TableSetupColumn("Parent Id");
		TableSetupColumn("Private Bytes");
		TableSetupColumn("Threads");
		TableSetupColumn("Handles");
		TableSetupColumn("Working Set");
		TableSetupColumn("Path", ImGuiTableColumnFlags_None);
		TableSetupColumn("Attributes");

		TableHeadersRow();

		if (IsKeyPressed(ImGuiKey_Space)) {
			TogglePause();
		}

		// Poll async remote fetch result (non-blocking)
		if (RemoteClient::IsConnected()) {
			if (_remoteFetchPending && _remoteFuture.valid() &&
				_remoteFuture.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
				auto netProcs = _remoteFuture.get();
				_remoteFetchPending = false;
				_processes.clear();
				_processes.reserve(netProcs.size());
				_processesEx.clear();
				for (auto& np : netProcs)
					_processes.push_back(ProcessInfoFromNet(np));
				if (_specs)
					DoSort(_specs->ColumnIndex, _specs->SortDirection == ImGuiSortDirection_Ascending);
				MarkUpdated();
			}
			if (IsUpdateDue() && !_remoteFetchPending) {
				_remoteFuture = std::async(std::launch::async, []() {
					return RemoteClient::GetProcesses();
				});
				_remoteFetchPending = true;
			}
		}
		else if (IsUpdateDue()) {
			auto empty = _processes.empty();
			if (empty) {
				_processes.reserve(1024);
				_processesEx.reserve(1024);
			}
			_pm.EnumProcesses();
			if (empty) {
				_processes = _pm.GetProcesses();
			}
			else {
				DoUpdate();
			}
			if (_specs)
				DoSort(_specs->ColumnIndex, _specs->SortDirection == ImGuiSortDirection_Ascending);
			LoggerView::AddLog(LoggerView::UserModeLog, "Enumerated %zu processes", _processes.size());
			MarkUpdated();
		}


		auto filter = GetFilterTextLower();
		std::vector<int> indices;
		indices.reserve(_processes.size());

		auto count = static_cast<int>(_processes.size());
		for (int i = 0; i < count; i++) {
			const auto& p = _processes[i];
			auto* px = remote ? nullptr : &GetProcessInfoEx(p.get());
			if (px)
				px->Filtered = false;
			if (px && px->Update()) {
				// process terminated
				_processesEx.erase(p->Key);
				_processes.erase(_processes.begin() + i);
				i--;
				count--;
				continue;
			}
			if (!filter.IsEmpty()) {
				CString name(p->GetImageName().c_str());
				name.MakeLower();
				if (name.Find(filter) < 0) {
					if (px)
						px->Filtered = true;
					continue;
				}
			}
			indices.push_back(i);
		}

		auto specs = TableGetSortSpecs();
		if (specs && specs->SpecsDirty) {
			_specs = specs->Specs;
			DoSort(_specs->ColumnIndex, _specs->SortDirection == ImGuiSortDirection_Ascending);
			specs->SpecsDirty = false;
		}
		USES_CONVERSION;
		ImGuiListClipper clipper;

		count = static_cast<int>(indices.size());
		clipper.Begin(count);
		auto special = false;
		static char buffer[64];
		CStringA str;

		int popCount = 3;
		static bool selected = false;

		auto orgBackColor = GetStyle().Colors[ImGuiCol_TableRowBg];

		if (_killFailed) {
			if (MessageBoxResult::StillOpen != SimpleMessageBox::ShowModal("Kill Process", "Failed to kill process!"))
				_killFailed = false;
		}
		while (clipper.Step()) {
			int iconBudget = clipper.DisplayEnd - clipper.DisplayStart;
			for (int j = clipper.DisplayStart; j < clipper.DisplayEnd; j++) {
				int i = indices[j];
				auto& p = _processes[i];
				auto* px = remote ? nullptr : &GetProcessInfoEx(p.get());
				if (px && px->Filtered) {
					clipper.ItemsCount--;
					continue;
				}
				TableNextRow();
				if (px) {
					auto [rowColor, textColor] = px->GetColors(_pm);
					(void)textColor;
					auto useRowColor = rowColor.x >= 0.0f;
					if (useRowColor)
						TableSetBgColor(ImGuiTableBgTarget_RowBg0, GetColorU32(rowColor));
				}

				TableSetColumnIndex(0);
				if (!remote) {
					if (auto icon = px->Icon(iconBudget-- > 0); icon != nullptr)
						Image(icon, ImVec2(16, 16));
					else
						Dummy(ImVec2(16, 16));
				}
				else
					Dummy(ImVec2(16, 16));
				SameLine();
				str.Format("%ws##%d", p->GetImageName().c_str(), i);
				Selectable(str, _selectedProcess == p, ImGuiSelectableFlags_SpanAllColumns);
				if (IsItemClicked())
					_selectedProcess = p;
				if (!remote && IsItemHovered() && IsMouseDoubleClicked(ImGuiMouseButton_Left))
					GetOrAddProcessProperties(p);
				if (IsItemClicked(ImGuiMouseButton_Right))
					_selectedProcess = p;

				// Right-click context menu
				if (BeginPopupContextItem()) {
					auto* cpx = remote ? nullptr : &GetProcessInfoEx(p.get());
					if (BeginMenu("Copy")) {
						if (MenuItem("Process Name")) {
							char buf[256];
							sprintf_s(buf, "%ws", p->GetImageName().c_str());
							SetClipboardText(buf);
						}
						if (MenuItem("PID")) {
							char buf[32];
							sprintf_s(buf, "%u", p->Id);
							SetClipboardText(buf);
						}
						if (MenuItem("PID (hex)")) {
							char buf[32];
							sprintf_s(buf, "0x%X", p->Id);
							SetClipboardText(buf);
						}
						if (MenuItem("Image Path")) {
							char buf[MAX_PATH];
							auto path = remote ? p->GetNativeImagePath() : cpx->GetExecutablePath();
							sprintf_s(buf, "%ws", path.c_str());
							SetClipboardText(buf);
						}
						ImGui::EndMenu();
					}
					Separator();
					if (!remote) {
						BuildPriorityClassMenu(p.get());
						Separator();
						if (MenuItem("Open File Location")) {
							GotoFileLocation(p.get());
						}
						if (MenuItem("Properties")) {
							GetOrAddProcessProperties(p);
						}
						if (MenuItem("Analyze Process")) {
							GetOrAddProcessSecurityWindow(p);
						}
						Separator();
						if (MenuItem("Kill Process")) {
							_selectedProcess = p;
							bool success;
							if (TryKillProcess(p.get(), success) && !success)
								_killFailed = true;
						}
					}
					else {
						ImGui::TextDisabled("Remote session: actions unavailable");
					}
					EndPopup();
				}

				::StringCchPrintfA(buffer, sizeof(buffer), "##%d", i);

				TableSetColumnIndex(1);
				Text("%6u (0x%05X)", p->Id, p->Id);

				TableSetColumnIndex(2);
				Text("%ws", remote ? L"-" : px->UserName().c_str());

				TableSetColumnIndex(3);
				Text("%u", p->SessionId);

				TableSetColumnIndex(4);
				Text("%d", p->CPU);

				TableSetColumnIndex(5);
				Text("%u", p->ParentId);

				TableSetColumnIndex(6);
				Text("%s", FormatHelper::FormatWithCommas(static_cast<long long>(p->PrivatePageCount)).GetString());

				TableSetColumnIndex(7);
				Text("%u", p->ThreadCount);

				TableSetColumnIndex(8);
				Text("%u", p->HandleCount);

				TableSetColumnIndex(9);
				Text("%s", FormatHelper::FormatWithCommas(static_cast<long long>(p->WorkingSetSize)).GetString());

				TableSetColumnIndex(10);
				Text("%ws", remote ? p->GetNativeImagePath().c_str() : px->GetExecutablePath().c_str());

				TableSetColumnIndex(11);
				if (remote)
					TextUnformatted("-");
				else {
					auto attrs = ProcessAttributesToString(px->GetAttributes(_pm));
					Text("%s", attrs.GetString());
				}
			}
		}
		EndTable();
	}
}

void ProcessesView::BuildViewMenu() {
	if (BeginMenu("View")) {
		if (BeginMenu("Update Interval")) {
			auto interval = GetUpdateInterval();
			if (MenuItem("500 ms", nullptr, interval == 500))
				SetUpdateInterval(500);
			if (MenuItem("1 second", nullptr, interval == 1000))
				SetUpdateInterval(1000);
			if (MenuItem("2 seconds", nullptr, interval == 2000))
				SetUpdateInterval(2000);
			if (MenuItem("5 seconds", nullptr, interval == 5000))
				SetUpdateInterval(5000);
			Separator();
			if (MenuItem("Paused", "SPACE", interval == 0))
				TogglePause();
			ImGui::EndMenu();
		}
		ImGui::EndMenu();
	}
}

void ProcessesView::BuildProcessMenu() {
	if (BeginMenu("Process")) {
		if (_selectedProcess && !RemoteClient::IsConnected()) {
			BuildPriorityClassMenu(_selectedProcess.get());
			Separator();
		}
		if (MenuItem("Kill", "Delete", false, _selectedProcess != nullptr && !RemoteClient::IsConnected())) {
			bool success;
			if (TryKillProcess(_selectedProcess.get(), success) && !success)
				_killFailed = true;
		}
		//Separator();
		ImGui::EndMenu();
	}
}

void ProcessesView::BuildToolBar() {
	Separator();
	DrawFilterToolbar();

	SameLine();
	DrawUpdateIntervalToolbar();
	SameLine();
	bool open = Button("Colors", ImVec2(60, 0));
	if (open)
		OpenPopup("colors");

	if (BeginPopup("colors", ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) {
		auto& colors = Globals::Get().GetSettings().ProcessColors;

		for (auto& c : colors) {
			Checkbox(c.Name, &c.Enabled);
			SameLine(150);
			ColorEdit4("Background##" + c.Name, (float*)&c.Color, ImGuiColorEditFlags_NoInputs);
			SameLine();
			if (Button("Reset##" + c.Name))
				c.Color = c.DefaultColor;

			SameLine();
			ColorEdit4("Text##" + c.Name, (float*)&c.TextColor, ImGuiColorEditFlags_NoInputs);
			SameLine();
			if (Button("Reset##Text" + c.Name))
				c.TextColor = c.DefaultTextColor;
		}

		EndPopup();
	}
}

void ProcessesView::BuildPriorityClassMenu(WinSys::ProcessInfo* pi) {
	using namespace WinSys;

	auto process = Process::OpenById(pi->Id,
		ProcessAccessMask::QueryLimitedInformation | ProcessAccessMask::SetInformation);
	bool enabled = process != nullptr;
	if (!enabled)
		process = Process::OpenById(pi->Id, ProcessAccessMask::QueryLimitedInformation);

	ProcessPriorityClass pc;
	if (process)
	{
		pc = process->GetPriorityClass();

		if (BeginMenu("Priority")) {
			if (MenuItem("Idle (4)", nullptr, pc == ProcessPriorityClass::Idle, enabled && pc != ProcessPriorityClass::Idle))
				process->SetPriorityClass(ProcessPriorityClass::Idle);
			if (MenuItem("Below Normal (6)", nullptr, pc == ProcessPriorityClass::BelowNormal, enabled && pc != ProcessPriorityClass::BelowNormal))
				process->SetPriorityClass(ProcessPriorityClass::BelowNormal);
			if (MenuItem("Normal (8)", nullptr, pc == ProcessPriorityClass::Normal, enabled && pc != ProcessPriorityClass::Normal))
				process->SetPriorityClass(ProcessPriorityClass::Normal);
			if (MenuItem("Above Normal (10)", nullptr, pc == ProcessPriorityClass::AboveNormal, enabled && pc != ProcessPriorityClass::AboveNormal))
				process->SetPriorityClass(ProcessPriorityClass::AboveNormal);
			if (MenuItem("High (13)", nullptr, pc == ProcessPriorityClass::High, enabled && pc != ProcessPriorityClass::High))
				process->SetPriorityClass(ProcessPriorityClass::High);
			if (MenuItem("Real-time (24)", nullptr, pc == ProcessPriorityClass::Realtime, enabled && pc != ProcessPriorityClass::Realtime))
				process->SetPriorityClass(ProcessPriorityClass::Realtime);

			ImGui::EndMenu();
		}
	}
}

bool ProcessesView::GotoFileLocation(WinSys::ProcessInfo* pi) {
	ATLASSERT(pi);
	auto& px = GetProcessInfoEx(pi);
	auto& path = px.GetExecutablePath();
	auto bs = path.rfind(L'\\');
	if (bs == std::wstring::npos)
		return false;

	auto folder = path.substr(0, bs);
	return (INT_PTR)::ShellExecute(nullptr, L"open", L"explorer", (L"/select,\"" + path + L"\"").c_str(),
		nullptr, SW_SHOWDEFAULT) > 31;
}

void ProcessesView::TogglePause() {
	ViewBase::TogglePause();
}

void ProcessesView::BuildPropertiesWindow(ProcessProperties* props) {
	SetNextWindowSizeConstraints(ImVec2(400, 300), GetIO().DisplaySize);
	SetNextWindowSize(ImVec2(520, 480), ImGuiCond_Once);
	if (Begin(props->GetName().c_str(), &props->WindowOpen, ImGuiWindowFlags_None)) {
		if (RemoteClient::IsConnected()) {
			TextColored(ImVec4(0.0f, 1.0f, 0.5f, 1.0f), "Remote: %s", RemoteClient::GetConnectedAddress());
			Separator();
			TextWrapped("Remote process property inspection is not implemented yet.");
			TextDisabled("Local process handles and memory regions are suppressed during a remote session.");
			End();
			return;
		}

		auto* pi = props->GetProcess();
		if (ImGui::Button("Refresh##ProcessProperties")) {
			_pm.EnumProcesses();
			if (auto updated = _pm.GetProcessByKey(pi->Key); updated) {
				props->SetProcess(updated);
				LoggerView::AddLog(LoggerView::UserModeLog, "Refreshed properties for PID %u (%ws)", updated->Id, updated->GetImageName().c_str());
			}
			else {
				LoggerView::AddLog(LoggerView::UserModeLog, "Properties refresh could not find PID %u; the process may have exited", pi->Id);
			}
			_processesEx.erase(pi->Key);
			props->ForceRefreshMemoryRegions();
			props->ForceRefreshModules();
			props->ForceRefreshHandles();
		}
		ImGui::SameLine();
		ImGui::TextDisabled("Refresh this process window");
		ImGui::Separator();

		pi = props->GetProcess();
		auto& px = GetProcessInfoEx(pi);
		props->RefreshMemoryRegions();
		props->RefreshModules();
		props->RefreshHandles();

		auto process = WinSys::Process::OpenById(pi->Id, WinSys::ProcessAccessMask::QueryLimitedInformation);

		// Header with icon and process name
		if (auto icon = px.Icon(); icon != nullptr) {
			Image(icon, ImVec2(32, 32));
			SameLine();
		}
		CStringA header;
		header.Format("%ws  (PID: %u)", pi->GetImageName().c_str(), pi->Id);
		SetCursorPosY(GetCursorPosY() + 6);
		Text("%s", header.GetString());
		Separator();

		if (BeginTabBar("##PropTabs")) {
			// ---- General tab ----
			if (BeginTabItem("General")) {
				if (BeginTable("##General", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
					TableSetupColumn("Field", ImGuiTableColumnFlags_WidthFixed, 180.0f);
					TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);

					auto Row = [](const char* field, const char* fmt, ...) {
						TableNextRow();
						TableSetColumnIndex(0);
						TextUnformatted(field);
						TableSetColumnIndex(1);
						va_list args;
						va_start(args, fmt);
						TextV(fmt, args);
						va_end(args);
					};

					Row("Image Name", "%ws", pi->GetImageName().c_str());
					Row("Process ID", "%u (0x%X)", pi->Id, pi->Id);
					Row("Parent Process ID", "%u", pi->ParentId);
					Row("Session ID", "%u", pi->SessionId);
					Row("Base Priority", "%d", pi->BasePriority);
					Row("Image Path", "%ws", px.GetExecutablePath().c_str());
					Row("User", "%ws", px.UserName().c_str());

					if (process) {
						auto cmdLine = process->GetCommandLine();
						if (!cmdLine.empty())
							Row("Command Line", "%ws", cmdLine.c_str());

						auto il = process->GetIntegrityLevel();
						const char* ilStr = "Unknown";
						switch (il) {
						case WinSys::IntegrityLevel::Untrusted: ilStr = "Untrusted"; break;
						case WinSys::IntegrityLevel::Low: ilStr = "Low"; break;
						case WinSys::IntegrityLevel::Medium: ilStr = "Medium"; break;
						case WinSys::IntegrityLevel::MediumPlus: ilStr = "Medium+"; break;
						case WinSys::IntegrityLevel::High: ilStr = "High"; break;
						case WinSys::IntegrityLevel::System: ilStr = "System"; break;
						case WinSys::IntegrityLevel::Protected: ilStr = "Protected"; break;
						}
						Row("Integrity Level", "%s", ilStr);

						Row("Elevated", "%s", process->IsElevated() ? "Yes" : "No");
						Row("WoW64", "%s", process->IsWow64Process() ? "Yes" : "No");
						Row(".NET Managed", "%s", process->IsManaged() ? "Yes" : "No");
						Row("Immersive", "%s", process->IsImmersive() ? "Yes" : "No");

						auto prot = process->GetProtection();
						if (prot.has_value() && prot->Level != 0)
							Row("Protection", "0x%02X", prot->Level);

						auto pc = process->GetPriorityClass();
						const char* pcStr = "Unknown";
						switch (pc) {
						case WinSys::ProcessPriorityClass::Idle: pcStr = "Idle (4)"; break;
						case WinSys::ProcessPriorityClass::BelowNormal: pcStr = "Below Normal (6)"; break;
						case WinSys::ProcessPriorityClass::Normal: pcStr = "Normal (8)"; break;
						case WinSys::ProcessPriorityClass::AboveNormal: pcStr = "Above Normal (10)"; break;
						case WinSys::ProcessPriorityClass::High: pcStr = "High (13)"; break;
						case WinSys::ProcessPriorityClass::Realtime: pcStr = "Real-time (24)"; break;
						}
						Row("Priority Class", "%s", pcStr);
						Row("Memory Priority", "%d", process->GetMemoryPriority());
						Row("I/O Priority", "%d", (int)process->GetIoPriority());
					}

					if (pi->CreateTime != 0) {
						FILETIME ft;
						ft.dwLowDateTime = (DWORD)(pi->CreateTime & 0xFFFFFFFF);
						ft.dwHighDateTime = (DWORD)(pi->CreateTime >> 32);
						SYSTEMTIME st, localSt;
						if (FileTimeToSystemTime(&ft, &st) && SystemTimeToTzSpecificLocalTime(nullptr, &st, &localSt)) {
							Row("Created", "%02d/%02d/%04d %02d:%02d:%02d",
								localSt.wMonth, localSt.wDay, localSt.wYear,
								localSt.wHour, localSt.wMinute, localSt.wSecond);
						}
					}

					auto attrs = ProcessAttributesToString(px.GetAttributes(_pm));
					if (!attrs.IsEmpty())
						Row("Attributes", "%s", attrs.GetString());

					EndTable();
				}
				EndTabItem();
			}

			// ---- Statistics tab ----
			if (BeginTabItem("Statistics")) {
				if (BeginTable("##Stats", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
					TableSetupColumn("Field", ImGuiTableColumnFlags_WidthFixed, 200.0f);
					TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);

					auto Row = [](const char* field, const char* fmt, ...) {
						TableNextRow();
						TableSetColumnIndex(0);
						TextUnformatted(field);
						TableSetColumnIndex(1);
						va_list args;
						va_start(args, fmt);
						TextV(fmt, args);
						va_end(args);
					};

					Row("Threads", "%u", pi->ThreadCount);
					Row("Peak Threads", "%u", pi->PeakThreads);
					Row("Handles", "%u", pi->HandleCount);
					Row("Page Faults", "%s", FormatHelper::FormatWithCommas(pi->PageFaultCount).GetString());
					Row("Hard Faults", "%s", FormatHelper::FormatWithCommas(pi->HardFaultCount).GetString());
					Separator();
					Row("CPU Time (User)", "%ws", FormatHelper::TimeSpanToString(pi->UserTime).GetString());
					Row("CPU Time (Kernel)", "%ws", FormatHelper::TimeSpanToString(pi->KernelTime).GetString());

					EndTable();
				}
				EndTabItem();
			}

			// ---- Memory tab ----
			if (BeginTabItem("Memory")) {
				auto const& regions = props->GetMemoryRegions();
				auto mapStats = ComputeAddressMapStats(regions);

				TextUnformatted("Virtual Address Map");
				SameLine();
				TextDisabled("| Regions: %zu  Committed: %s  Range: 0x%llX - 0x%llX",
					mapStats.RegionCount,
					FormatHelper::FormatWithCommas(mapStats.CommittedBytes).GetString(),
					mapStats.Lowest,
					mapStats.Highest);
				auto hoveredRegion = DrawProcessAddressMap("##ProcessAddressMap", regions, ImVec2(GetContentRegionAvail().x, 156.0f));
				TextDisabled("Color: private=green, mapped=blue, image=dark green, execute=orange/red, reserve/free=gray");
				if (hoveredRegion && ImGui::BeginTooltip()) {
					auto base = reinterpret_cast<ULONG64>(hoveredRegion->BaseAddress);
					auto end = base + static_cast<ULONG64>(hoveredRegion->RegionSize);
					ImGui::Text("Base: 0x%llX", base);
					ImGui::Text("End: 0x%llX", end);
					ImGui::Text("Size: %s", FormatHelper::FormatWithCommas(static_cast<ULONG64>(hoveredRegion->RegionSize)).GetString());
					ImGui::Text("State: %s", StateToString(hoveredRegion->State));
					ImGui::Text("Type: %s", TypeToString(hoveredRegion->Type));
					ImGui::Text("Protect: %s", ProtectToString(hoveredRegion->Protect).c_str());
					ImGui::Text("Alloc Protect: %s", ProtectToString(hoveredRegion->AllocationProtect).c_str());
					ImGui::Text("Allocation Base: 0x%llX", reinterpret_cast<ULONG64>(hoveredRegion->AllocationBase));
					ImGui::EndTooltip();
				}
				Spacing();

				if (BeginTable("##Mem", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
					TableSetupColumn("Field", ImGuiTableColumnFlags_WidthFixed, 200.0f);
					TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);

					auto Row = [](const char* field, const char* fmt, ...) {
						TableNextRow();
						TableSetColumnIndex(0);
						TextUnformatted(field);
						TableSetColumnIndex(1);
						va_list args;
						va_start(args, fmt);
						TextV(fmt, args);
						va_end(args);
					};

					Row("Private Bytes", "%s", FormatHelper::FormatWithCommas(static_cast<long long>(pi->PrivatePageCount)).GetString());
					Row("Working Set", "%s", FormatHelper::FormatWithCommas(static_cast<long long>(pi->WorkingSetSize)).GetString());
					Row("Peak Working Set", "%s", FormatHelper::FormatWithCommas(static_cast<long long>(pi->PeakWorkingSetSize)).GetString());
					Row("Working Set (Private)", "%s", FormatHelper::FormatWithCommas(pi->WorkingSetPrivateSize).GetString());
					Row("Virtual Size", "%s", FormatHelper::FormatWithCommas(static_cast<long long>(pi->VirtualSize)).GetString());
					Row("Peak Virtual Size", "%s", FormatHelper::FormatWithCommas(static_cast<long long>(pi->PeakVirtualSize)).GetString());
					Row("Paged Pool Usage", "%s", FormatHelper::FormatWithCommas(static_cast<long long>(pi->PagedPoolUsage)).GetString());
					Row("Peak Paged Pool", "%s", FormatHelper::FormatWithCommas(static_cast<long long>(pi->PeakPagedPoolUsage)).GetString());
					Row("Non-Paged Pool Usage", "%s", FormatHelper::FormatWithCommas(static_cast<long long>(pi->NonPagedPoolUsage)).GetString());
					Row("Peak Non-Paged Pool", "%s", FormatHelper::FormatWithCommas(static_cast<long long>(pi->PeakNonPagedPoolUsage)).GetString());
					Row("Pagefile Usage", "%s", FormatHelper::FormatWithCommas(static_cast<long long>(pi->PagefileUsage)).GetString());
					Row("Peak Pagefile Usage", "%s", FormatHelper::FormatWithCommas(static_cast<long long>(pi->PeakPagefileUsage)).GetString());

					EndTable();
				}
				EndTabItem();
			}

			if (BeginTabItem("DLLs")) {
				auto const& modules = props->GetModules();
				size_t onDiskCount = 0;
				size_t memoryOnlyCount = 0;
				size_t sideLoadCount = 0;
				for (auto const& module : modules) {
					if (module.SideLoadCandidate)
						sideLoadCount++;
					else if (module.ExistsOnDisk)
						onDiskCount++;
					else
						memoryOnlyCount++;
				}

				Text("Loaded DLLs: %zu", modules.size());
				SameLine();
				TextDisabled("|");
				SameLine();
				TextColored(ImVec4(0.35f, 1.0f, 0.35f, 1.0f), "Disk-backed: %zu", onDiskCount);
				SameLine();
				TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f), "Memory-only/Deleted: %zu", memoryOnlyCount);
				SameLine();
				TextColored(ImVec4(1.0f, 0.75f, 0.25f, 1.0f), "Side-load Candidates: %zu", sideLoadCount);
				Separator();
				TextDisabled("Green = disk-backed, red = missing on disk, amber = app-local DLL with a common system DLL name");

				if (BeginTable("##Dlls", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable)) {
					TableSetupColumn("Module", ImGuiTableColumnFlags_WidthFixed, 180.0f);
					TableSetupColumn("Base", ImGuiTableColumnFlags_WidthFixed, 120.0f);
					TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 90.0f);
					TableSetupColumn("State", ImGuiTableColumnFlags_WidthFixed, 150.0f);
					TableSetupColumn("Path");
					TableSetupColumn("Notes", ImGuiTableColumnFlags_WidthFixed, 180.0f);
					TableHeadersRow();

					for (auto const& module : modules) {
						TableNextRow();
						if (module.SideLoadCandidate)
							TableSetBgColor(ImGuiTableBgTarget_RowBg0, IM_COL32(180, 120, 10, 70));
						else if (!module.ExistsOnDisk)
							TableSetBgColor(ImGuiTableBgTarget_RowBg0, IM_COL32(180, 40, 40, 70));
						else
							TableSetBgColor(ImGuiTableBgTarget_RowBg0, IM_COL32(40, 120, 40, 40));

						TableSetColumnIndex(0);
						Text("%s", module.ModuleName.c_str());
						TableSetColumnIndex(1);
						Text("0x%llX", module.BaseAddress);
						TableSetColumnIndex(2);
						Text("0x%X", module.Size);
						TableSetColumnIndex(3);
						if (module.SideLoadCandidate)
							TextColored(ImVec4(1.0f, 0.75f, 0.25f, 1.0f), "Side-load candidate");
						else if (module.ExistsOnDisk)
							TextColored(ImVec4(0.35f, 1.0f, 0.35f, 1.0f), "On disk");
						else
							TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f), "Memory only");
						TableSetColumnIndex(4);
						Text("%s", module.ModulePath.c_str());
						TableSetColumnIndex(5);
						if (module.SideLoadCandidate)
							TextUnformatted("App-local module with system-like name");
						else if (!module.ExistsOnDisk)
							TextUnformatted("Mapped but backing file is gone");
						else
							TextUnformatted("-");
					}

					EndTable();
				}
				EndTabItem();
			}

// ---- Handles tab ----
			if (BeginTabItem("Handles")) {
				if (props->IsHandleRefreshPending()) {
					TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "Enumerating handles...");
				}
				else {
					const auto& handles = props->GetHandles();

					// Summary
					int suspicious = 0;
					for (const auto& h : handles)
						if (h.Suspicious) suspicious++;

					Text("Total: %zu", handles.size());
					SameLine(0, 20);
					if (suspicious > 0)
						TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "Suspicious: %d", suspicious);
					else
						TextColored(ImVec4(0.3f, 1.0f, 0.3f, 1.0f), "No suspicious handles");

					// Filters
					static bool showSuspiciousOnly = false;
					static char typeFilter[64]{};
					Checkbox("Suspicious only", &showSuspiciousOnly);
					SameLine(0, 20);
					SetNextItemWidth(150);
					InputText("Type Filter##handles", typeFilter, sizeof(typeFilter));
					Separator();

					std::string typeFilterLower;
					if (typeFilter[0]) {
						typeFilterLower = typeFilter;
						for (auto& c : typeFilterLower) c = (char)tolower((unsigned char)c);
					}

					if (BeginTable("##ProcHandles", 6,
						ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
						ImGuiTableFlags_Resizable | ImGuiTableFlags_Reorderable)) {
						TableSetupScrollFreeze(0, 1);
						TableSetupColumn("Handle", ImGuiTableColumnFlags_WidthFixed, 70);
						TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 100);
						TableSetupColumn("Access", ImGuiTableColumnFlags_WidthFixed, 220);
						TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
						TableSetupColumn("Security", ImGuiTableColumnFlags_WidthFixed, 280);
						TableSetupColumn("Object", ImGuiTableColumnFlags_WidthFixed, 130);
						TableHeadersRow();

						for (const auto& h : handles) {
							if (showSuspiciousOnly && !h.Suspicious)
								continue;
							if (!typeFilterLower.empty()) {
								std::string typeLower = h.TypeName;
								for (auto& c : typeLower) c = (char)tolower((unsigned char)c);
								if (typeLower.find(typeFilterLower) == std::string::npos)
									continue;
							}

							TableNextRow();
							if (h.Suspicious)
								TableSetBgColor(ImGuiTableBgTarget_RowBg1, GetColorU32(ImVec4(0.5f, 0.1f, 0.1f, 0.4f)));

							TableSetColumnIndex(0);
							Text("0x%llX", h.HandleValue);
							TableSetColumnIndex(1);
							TextUnformatted(h.TypeName.c_str());
							TableSetColumnIndex(2);
							TextUnformatted(h.DecodedAccess.c_str());
							TableSetColumnIndex(3);
							TextUnformatted(h.ObjectName.empty() ? "" : h.ObjectName.c_str());
							TableSetColumnIndex(4);
							if (h.Suspicious)
								TextColored(ImVec4(1.0f, 0.4f, 0.3f, 1.0f), "%s", h.SecurityNote.c_str());
							TableSetColumnIndex(5);
							Text("0x%llX", h.Object);
						}
						EndTable();
					}
				}
				EndTabItem();
			}

// ---- I/O tab ----
			if (BeginTabItem("I/O")) {
				if (BeginTable("##IO", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
					TableSetupColumn("Field", ImGuiTableColumnFlags_WidthFixed, 200.0f);
					TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);

					auto Row = [](const char* field, const char* fmt, ...) {
						TableNextRow();
						TableSetColumnIndex(0);
						TextUnformatted(field);
						TableSetColumnIndex(1);
						va_list args;
						va_start(args, fmt);
						TextV(fmt, args);
						va_end(args);
					};

					Row("Read Operations", "%s", FormatHelper::FormatWithCommas(pi->ReadOperationCount).GetString());
					Row("Read Transfer", "%s bytes", FormatHelper::FormatWithCommas(pi->ReadTransferCount).GetString());
					Row("Write Operations", "%s", FormatHelper::FormatWithCommas(pi->WriteOperationCount).GetString());
					Row("Write Transfer", "%s bytes", FormatHelper::FormatWithCommas(pi->WriteTransferCount).GetString());
					Row("Other Operations", "%s", FormatHelper::FormatWithCommas(pi->OtherOperationCount).GetString());
					Row("Other Transfer", "%s bytes", FormatHelper::FormatWithCommas(pi->OtherTransferCount).GetString());

					EndTable();
				}
				EndTabItem();
			}

			EndTabBar();
		}
	}
	End();
}

void ProcessesView::BuildSecurityWindow(ProcessSecurityWindow* window) {
	SetNextWindowSizeConstraints(ImVec2(640, 420), GetIO().DisplaySize);
	SetNextWindowSize(ImVec2(900, 620), ImGuiCond_Once);
	if (Begin(window->GetName().c_str(), &window->WindowOpen, ImGuiWindowFlags_None)) {
		window->View.BuildWindow();
	}
	End();
}

std::shared_ptr<ProcessProperties> ProcessesView::GetProcessProperties(WinSys::ProcessInfo* pi) {
	auto it = _processProperties.find(pi->Key);
	return it == _processProperties.end() ? nullptr : it->second;
}

std::shared_ptr<ProcessProperties> ProcessesView::GetOrAddProcessProperties(const std::shared_ptr<WinSys::ProcessInfo>& pi) {
	auto props = GetProcessProperties(pi.get());
	if (props == nullptr) {
		CStringA name;
		name.Format("%ws (%u) Properties##%lld", pi->GetImageName().c_str(), pi->Id, pi->CreateTime);
		props = std::make_shared<ProcessProperties>(std::string(name), pi);
		_processProperties.insert({ pi->Key, props });
		//_tm.AddWindow(props);
	}
	return props;
}

std::shared_ptr<ProcessesView::ProcessSecurityWindow> ProcessesView::GetProcessSecurityWindow(WinSys::ProcessInfo* pi) {
	auto it = _processSecurityWindows.find(pi->Key);
	return it == _processSecurityWindows.end() ? nullptr : it->second;
}

std::shared_ptr<ProcessesView::ProcessSecurityWindow> ProcessesView::GetOrAddProcessSecurityWindow(const std::shared_ptr<WinSys::ProcessInfo>& pi) {
	auto window = GetProcessSecurityWindow(pi.get());
	if (window == nullptr) {
		CStringA name;
		name.Format("%ws (%u) Analyze Process##%lld", pi->GetImageName().c_str(), pi->Id, pi->CreateTime);
		window = std::make_shared<ProcessSecurityWindow>(std::string(name), pi);
		window->View.SetTargetPid(pi->Id);
		window->View.RefreshNow();
		_processSecurityWindows.insert({ pi->Key, window });
	}
	return window;
}

CStringA ProcessesView::ProcessAttributesToString(ProcessAttributes attributes) {
	CStringA text;

	static const struct {
		ProcessAttributes Attribute;
		const char* Text;
	} attribs[] = {
		{ ProcessAttributes::Managed, "Managed" },
		{ ProcessAttributes::Immersive, "Immersive" },
		{ ProcessAttributes::Protected, "Protected" },
		{ ProcessAttributes::Secure, "Secure" },
		{ ProcessAttributes::Service, "Service" },
		{ ProcessAttributes::InJob, "In Job" },
	};

	for (auto& item : attribs)
		if ((item.Attribute & attributes) == item.Attribute)
			text += CStringA(item.Text) + ", ";
	if (!text.IsEmpty())
		text = text.Mid(0, text.GetLength() - 2);
	return text;
}
