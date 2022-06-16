#include "pch.h"
#include "ShortcutControl.h"
#if __has_include("ShortcutControl.g.cpp")
#include "ShortcutControl.g.cpp"
#endif

using namespace winrt;
using namespace Windows::UI::Xaml::Controls;


namespace winrt::Magpie::App::implementation {

ShortcutControl* ShortcutControl::_that = nullptr;

ShortcutControl::ShortcutControl() {
	InitializeComponent();

	_shortcutDialog.Title(box_value(L"激活快捷键"));
	_shortcutDialog.Content(_shortcutDialogContent);
	_shortcutDialog.PrimaryButtonText(L"保存");
	_shortcutDialog.CloseButtonText(L"取消");
	_shortcutDialog.DefaultButton(ContentDialogButton::Primary);
	_shortcutDialog.Opened({ this, &ShortcutControl::ShortcutDialog_Opened });
	_shortcutDialog.Closing({ this, &ShortcutControl::ShortcutDialog_Closing });

	_hotkey.Win(true);
	_hotkey.Alt(true);
	KeysControl().ItemsSource(_hotkey.GetKeyList());
}

IAsyncAction ShortcutControl::EditButton_Click(IInspectable const&, RoutedEventArgs const&) {
	_previewHotkey.CopyFrom(_hotkey);
	_shortcutDialogContent.Keys(_previewHotkey.GetKeyList());

	_shortcutDialog.XamlRoot(XamlRoot());
	_shortcutDialog.RequestedTheme(ActualTheme());

	// 防止快速点击时崩溃
	static bool showing = false;
	if (showing) {
		co_return;
	}
	showing = true;
	co_await _shortcutDialog.ShowAsync();
	showing = false;
}

void ShortcutControl::ShortcutDialog_Opened(ContentDialog const&, ContentDialogOpenedEventArgs const&) {
	_that = this;
	_keyboardHook = SetWindowsHookEx(WH_KEYBOARD_LL, _LowLevelKeyboardProc, NULL, 0);
}

void ShortcutControl::ShortcutDialog_Closing(ContentDialog const&, ContentDialogClosingEventArgs const& args) {
	UnhookWindowsHookEx(_keyboardHook);

	if (args.Result() == ContentDialogResult::Primary) {
		_hotkey.CopyFrom(_previewHotkey);
		KeysControl().ItemsSource(_hotkey.GetKeyList());
	}
}

bool CheckVirtualKey(DWORD vkCode) {
	return (vkCode >= 'A' && vkCode <= 'Z')	// 字母
		|| (vkCode >= '0' && vkCode <= '9')	// 数字（顶部）
		|| (vkCode >= VK_NUMPAD0 && vkCode <= VK_NUMPAD9)	// 数字（小键盘）
		|| (vkCode >= VK_F1 && vkCode <= VK_F24)			// F1~F24
		|| (vkCode >= VK_SPACE && vkCode <= VK_DOWN)		// 空格、Page Up/Down、End、Home、方向键
		|| vkCode == VK_INSERT		// Insert
		|| vkCode == VK_DELETE		// Delete
		|| vkCode == VK_ADD			// 加（小键盘）
		|| vkCode == VK_SUBTRACT	// 减（小键盘）
		|| vkCode == VK_MULTIPLY	// 乘（小键盘）
		|| vkCode == VK_DIVIDE		// 除（小键盘）
		|| (vkCode >= VK_OEM_1 && vkCode <= VK_OEM_3)	// 分号、等号、逗号、-、句号、/、`
		|| (vkCode >= VK_OEM_4 && vkCode <= VK_OEM_7)	// [、\、]、'
		|| vkCode == VK_BACK		// Backspace
		|| vkCode == VK_RETURN;		// 回车
}

LRESULT ShortcutControl::_LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
	if (nCode != HC_ACTION || !_that) {
		return CallNextHookEx(NULL, nCode, wParam, lParam);
	}

	// 只有位于前台时才监听按键
	App app = Application::Current().as<App>();
	if (GetForegroundWindow() != (HWND)app.HwndHost()) {
		return CallNextHookEx(NULL, nCode, wParam, lParam);
	}

	bool isKeyDown = wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN;

	DWORD code = ((KBDLLHOOKSTRUCT*)lParam)->vkCode;
	switch (code) {
	case VK_LWIN:
	case VK_RWIN:
		_that->_pressedKeys.Win(isKeyDown);
		break;
	case VK_CONTROL:
	case VK_LCONTROL:
	case VK_RCONTROL:
		_that->_pressedKeys.Ctrl(isKeyDown);
		break;
	case VK_SHIFT:
	case VK_LSHIFT:
	case VK_RSHIFT:
		_that->_pressedKeys.Shift(isKeyDown);
		break;
	case VK_MENU:
	case VK_LMENU:
	case VK_RMENU:
		_that->_pressedKeys.Alt(isKeyDown);
		break;
	default:
	{
		if (CheckVirtualKey(code)) {
			if (isKeyDown) {
				_that->_pressedKeys.Code(code);
			} else {
				_that->_pressedKeys.Code(0);
			}
		} else {
			isKeyDown = false;
		}
		
		break;
	}
	}

	if (isKeyDown) {
		_that->_previewHotkey.CopyFrom(_that->_pressedKeys);
		_that->_shortcutDialogContent.Keys(_that->_previewHotkey.GetKeyList());

		_that->_shortcutDialogContent.IsError(!_that->_previewHotkey.Check());
	}

	return -1;
}

}
