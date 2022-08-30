#include "pch.h"
#include "MagService.h"
#include "HotkeyService.h"
#include "Win32Utils.h"
#include "AppSettings.h"
#include "ScalingProfileService.h"

using namespace Magpie::Core;


namespace winrt::Magpie::UI {

void MagService::Initialize() {
	_dispatcher = CoreWindow::GetForCurrentThread().Dispatcher();

	_timer.Interval(TimeSpan(std::chrono::milliseconds(25)));
	_timerTickRevoker = _timer.Tick(
		auto_revoke,
		{ this, &MagService::_Timer_Tick }
	);

	AppSettings::Get().IsAutoRestoreChanged({ this, &MagService::_Settings_IsAutoRestoreChanged });
	_magRuntime.IsRunningChanged({ this, &MagService::_MagRuntime_IsRunningChanged });

	HotkeyService::Get().HotkeyPressed(
		{ this, &MagService::_HotkeyService_HotkeyPressed }
	);

	_UpdateIsAutoRestore();
}

void MagService::StartCountdown() {
	if (_tickingDownCount != 0) {
		return;
	}

	_tickingDownCount = AppSettings::Get().DownCount();
	_timerStartTimePoint = std::chrono::steady_clock::now();
	_timer.Start();
	_isCountingDownChangedEvent(true);
}

void MagService::StopCountdown() {
	if (_tickingDownCount == 0) {
		return;
	}

	_tickingDownCount = 0;
	_timer.Stop();
	_isCountingDownChangedEvent(false);
}

float MagService::CountdownLeft() const noexcept {
	using namespace std::chrono;

	if (!IsCountingDown()) {
		return 0.0f;
	}

	// DispatcherTimer 误差很大，因此我们自己计算剩余时间
	auto now = steady_clock::now();
	int timeLeft = (int)duration_cast<milliseconds>(_timerStartTimePoint + seconds(_tickingDownCount) - now).count();
	return timeLeft / 1000.0f;
}

void MagService::ClearWndToRestore() {
	if (_wndToRestore == 0) {
		return;
	}

	_wndToRestore = 0;
	_wndToRestoreChangedEvent(_wndToRestore);
}

void MagService::_HotkeyService_HotkeyPressed(HotkeyAction action) {
	switch (action) {
	case HotkeyAction::Scale:
	{
		if (_magRuntime.IsRunning()) {
			_magRuntime.Stop();
			return;
		}

		_StartScale();
		break;
	}
	case HotkeyAction::Overlay:
	{
		if (_magRuntime.IsRunning()) {
			_magRuntime.ToggleOverlay();
			return;
		}
		break;
	}
	default:
		break;
	}
}

void MagService::_Timer_Tick(IInspectable const&, IInspectable const&) {
	float timeLeft = CountdownLeft();

	// 剩余时间在 10 ms 以内计时结束
	if (timeLeft < 0.01) {
		StopCountdown();
		_StartScale();
		return;
	}

	_countdownTickEvent(timeLeft);
}

void MagService::_Settings_IsAutoRestoreChanged(bool) {
	_UpdateIsAutoRestore();
}

IAsyncAction MagService::_MagRuntime_IsRunningChanged(bool) {
	co_await _dispatcher.RunAsync(CoreDispatcherPriority::Normal, [this]() {
		bool isRunning = _magRuntime.IsRunning();
		if (isRunning) {
			StopCountdown();

			if (AppSettings::Get().IsAutoRestore()) {
				_curSrcWnd = _magRuntime.HwndSrc();
				_wndToRestore = 0;
				_wndToRestoreChangedEvent(_wndToRestore);
			}
		} else {
			HWND hwndMain = (HWND)Application::Current().as<Magpie::UI::App>().HwndMain();

			// 必须在主线程还原主窗口样式
			// 见 FrameSourceBase::~FrameSourceBase
			LONG_PTR style = GetWindowLongPtr(hwndMain, GWL_STYLE);
			if (!(style & WS_THICKFRAME)) {
				SetWindowLongPtr(hwndMain, GWL_STYLE, style | WS_THICKFRAME);
				SetWindowPos(hwndMain, 0, 0, 0, 0, 0,
					SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
			}

			if (AppSettings::Get().IsAutoRestore()) {
				// 退出全屏之后前台窗口不变则不必记忆
				if (IsWindow(_curSrcWnd) && GetForegroundWindow() != _curSrcWnd) {
					_wndToRestore = _curSrcWnd;
					_wndToRestoreChangedEvent(_wndToRestore);
				}

				_curSrcWnd = NULL;
			}
		}

		_isRunningChangedEvent(isRunning);
	});
}

void MagService::_UpdateIsAutoRestore() {
	if (AppSettings::Get().IsAutoRestore()) {
		// 立即生效，即使正处于缩放状态
		_curSrcWnd = _magRuntime.HwndSrc();

		// 监听前台窗口更改
		_hForegroundEventHook = SetWinEventHook(
			EVENT_SYSTEM_FOREGROUND,
			EVENT_SYSTEM_FOREGROUND,
			NULL,
			_WinEventProcCallback,
			0,
			0,
			WINEVENT_OUTOFCONTEXT
		);
		// 监听窗口销毁
		_hDestoryEventHook = SetWinEventHook(
			EVENT_OBJECT_DESTROY,
			EVENT_OBJECT_DESTROY,
			NULL,
			_WinEventProcCallback,
			0,
			0,
			WINEVENT_OUTOFCONTEXT
		);
	} else {
		_curSrcWnd = NULL;
		_wndToRestore = 0;
		_wndToRestoreChangedEvent(_wndToRestore);
		if (_hForegroundEventHook) {
			UnhookWinEvent(_hForegroundEventHook);
			_hForegroundEventHook = NULL;
		}
		if (_hDestoryEventHook) {
			UnhookWinEvent(_hDestoryEventHook);
			_hDestoryEventHook = NULL;
		}
	}
}

void MagService::_CheckForeground() {
	if (_wndToRestore == 0 || _magRuntime.IsRunning()) {
		return;
	}

	if (!IsWindow(_wndToRestore)) {
		_wndToRestore = 0;
		_wndToRestoreChangedEvent(_wndToRestore);
		return;
	}

	if (_wndToRestore != GetForegroundWindow()) {
		return;
	}

	_StartScale(_wndToRestore);
}

void MagService::_StartScale(HWND hWnd) {
	if (!hWnd) {
		hWnd = GetForegroundWindow();
	}

	if (hWnd && Win32Utils::GetWindowShowCmd(hWnd) != SW_NORMAL) {
		return;
	}

	const ScalingProfile& profile = ScalingProfileService::Get().GetProfileForWindow((HWND)hWnd);
	
	MagOptions options;
	options.graphicsAdapter = profile.graphicsAdapter;
	options.captureMode = profile.captureMode;
	options.multiMonitorUsage = profile.multiMonitorUsage;
	options.cursorInterpolationMode = profile.cursorInterpolationMode;
	options.flags = profile.flags;

	if (!profile.isCroppingEnabled) {
		options.cropping = profile.cropping;
	}

	switch (profile.cursorScaling) {
	case CursorScaling::x0_5:
		options.cursorScaling = 0.5;
		break;
	case CursorScaling::x0_75:
		options.cursorScaling = 0.75;
		break;
	case CursorScaling::NoScaling:
		options.cursorScaling = 1.0;
		break;
	case CursorScaling::x1_25:
		options.cursorScaling = 1.25;
		break;
	case CursorScaling::x1_5:
		options.cursorScaling = 1.5;
		break;
	case CursorScaling::x2:
		options.cursorScaling = 2.0;
		break;
	case CursorScaling::Source:
		// 0 或负值表示和源窗口缩放比例相同
		options.cursorScaling = 0;
		break;
	case CursorScaling::Custom:
		options.cursorScaling = profile.customCursorScaling;
		break;
	default:
		options.cursorScaling = 1.0;
		break;
	}

	// 应用全局配置
	AppSettings& settings = AppSettings::Get();
	options.IsBreakpointMode(settings.IsBreakpointMode());
	options.IsDisableEffectCache(settings.IsDisableEffectCache());
	options.IsSaveEffectSources(settings.IsSaveEffectSources());
	options.IsWarningsAreErrors(settings.IsWarningsAreErrors());
	options.IsSimulateExclusiveFullscreen(settings.IsSimulateExclusiveFullscreen());

	EffectOption& easu = options.effects.emplace_back();
	easu.name = L"FSR\\FSR_EASU";
	easu.scaleType = ScaleType::Fit;
	if (settings.IsInlineParams()) {
		easu.flags |= EffectOptionFlags::InlineParams;
	}
	EffectOption& rcas = options.effects.emplace_back();
	rcas.name = L"FSR\\FSR_RCAS";
	rcas.parameters[L"sharpness"] = 0.9;
	if (settings.IsInlineParams()) {
		rcas.flags |= EffectOptionFlags::InlineParams;
	}

	_magRuntime.Run(hWnd, options);
}

void MagService::_WinEventProcCallback(HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD, DWORD) {
	MagService::Get()._CheckForeground();
}

}