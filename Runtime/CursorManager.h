#pragma once
#include "pch.h"
#include "Utils.h"
#include "Renderable.h"
#include "Env.h"
#include "MonochromeCursorEffect.h"

using namespace D2D1;


// 处理光标的渲染
class CursorManager: public Renderable {
public:
	CursorManager() {
		if (Env::$instance->IsNoDisturb()) {
			return;
		}

		// 限制鼠标在窗口内
		// 静默的失败
		ClipCursor(&Env::$instance->GetSrcClient()), L"ClipCursor 失败";

		if (Env::$instance->IsAdjustCursorSpeed()) {
			// 设置鼠标移动速度
			//Debug::ThrowIfWin32Failed(
			SystemParametersInfo(SPI_GETMOUSESPEED, 0, &_cursorSpeed, 0);
			//	L"获取鼠标速度失败"
			//);

			const RECT& srcClient = Env::$instance->GetSrcClient();
			const D2D_RECT_F& destRect = Env::$instance->GetDestRect();
			float scaleX = (destRect.right - destRect.left) / (srcClient.right - srcClient.left);
			float scaleY = (destRect.bottom - destRect.top) / (srcClient.bottom - srcClient.top);

			long newSpeed = std::clamp(lroundf(_cursorSpeed / (scaleX + scaleY) * 2), 1L, 20L);
			//Debug::ThrowIfWin32Failed(
			SystemParametersInfo(SPI_SETMOUSESPEED, 0, (PVOID)(intptr_t)newSpeed, 0);
			//	L"设置鼠标速度失败"
			//);
		}

		MagInitialize();
		MagShowSystemCursor(FALSE);
	}

	CursorManager(const CursorManager&) = delete;
	CursorManager(CursorManager&&) = delete;

	~CursorManager() {
		if (Env::$instance->IsNoDisturb()) {
			return;
		}

		ClipCursor(nullptr);

		if (Env::$instance->IsAdjustCursorSpeed()) {
			SystemParametersInfo(SPI_SETMOUSESPEED, 0, (PVOID)(intptr_t)_cursorSpeed, 0);
		}

		MagShowSystemCursor(TRUE);
		MagUninitialize();
	}

private:
	struct CursorInfo {
		HCURSOR handle = NULL;
		ComPtr<ID2D1Bitmap> bmp = nullptr;
		int xHotSpot = 0;
		int yHotSpot = 0;
		int width = 0;
		int height = 0;
		bool isMonochrome = false;
	};

	CursorInfo* _cursorInfo = nullptr;
	D2D1_POINT_2L _targetScreenPos{};

public:
	ComPtr<ID2D1Image> RenderEffect(ComPtr<ID2D1Image> input) {
		_CalcCursorPos();

		if (!_cursorInfo || !_cursorInfo->isMonochrome) {
			return input;
		}

		if (!_monochromeCursorEffect) {
			Debug::ThrowIfComFailed(
				MonochromeCursorEffect::Register(Env::$instance->GetD2DFactory()),
				L"注册MonochromeCursorEffect失败"
			);
			Debug::ThrowIfComFailed(
				Env::$instance->GetD2DDC()->CreateEffect(CLSID_MAGPIE_MONOCHROME_CURSOR_EFFECT, &_monochromeCursorEffect),
				L"创建MonochromeCursorEffect失败"
			);
		}

		_monochromeCursorEffect->SetInput(0, input.Get());
		_monochromeCursorEffect->SetInput(1, _cursorInfo->bmp.Get());

		auto& destRect = Env::$instance->GetDestRect();
		_monochromeCursorEffect->SetValue(
			MonochromeCursorEffect::PROP_CURSOR_POS,
			D2D_VECTOR_2F{ FLOAT(_targetScreenPos.x) - destRect.left, FLOAT(_targetScreenPos.y) - destRect.top }
		);

		ComPtr<ID2D1Image> output;
		_monochromeCursorEffect->GetOutput(&output);
		return output;
	}

	void Render() override {
		if (!_cursorInfo || _cursorInfo->isMonochrome) {
			return;
		}

		D2D1_RECT_F cursorRect = {
			FLOAT(_targetScreenPos.x),
			FLOAT(_targetScreenPos.y),
			FLOAT(_targetScreenPos.x + _cursorInfo->width),
			FLOAT(_targetScreenPos.y + _cursorInfo->height)
		};

		Env::$instance->GetD2DDC()->DrawBitmap(_cursorInfo->bmp.Get(), &cursorRect);
	}

private:
	void _CalcCursorPos() {
		CURSORINFO ci{};
		ci.cbSize = sizeof(ci);
		//Debug::ThrowIfWin32Failed(
		GetCursorInfo(&ci);
		//	L"GetCursorInfo 失败"
		//);

		if (ci.hCursor == NULL || ci.flags != CURSOR_SHOWING) {
			_cursorInfo = nullptr;
			return;
		}

		auto it = _cursorMap.find(ci.hCursor);
		if (it != _cursorMap.end()) {
			_cursorInfo = &it->second;
		} else {
			try {
				// 未在映射中找到，创建新映射
				_ResolveCursor(ci.hCursor, ci.hCursor);

				_cursorInfo = &_cursorMap[ci.hCursor];
			} catch (...) {
				// 如果出错，不绘制光标
				_cursorInfo = nullptr;
				return;
			}
		}

		// 映射坐标
		// 鼠标坐标为整数，否则会出现模糊
		const RECT& srcClient = Env::$instance->GetSrcClient();
		const D2D_RECT_F& destRect = Env::$instance->GetDestRect();
		float scaleX = (destRect.right - destRect.left) / (srcClient.right - srcClient.left);
		float scaleY = (destRect.bottom - destRect.top) / (srcClient.bottom - srcClient.top);

		_targetScreenPos = {
			lroundf((ci.ptScreenPos.x - srcClient.left) * scaleX + destRect.left) - _cursorInfo->xHotSpot,
			lroundf((ci.ptScreenPos.y - srcClient.top) * scaleY + destRect.top) - _cursorInfo->yHotSpot
		};
	}

	ComPtr<ID2D1Bitmap> _CursorToD2DBitmap(HCURSOR hCursor) {
		assert(hCursor != NULL);

		IWICImagingFactory2* wicImgFactory = Env::$instance->GetWICImageFactory();

		ComPtr<IWICBitmap> wicCursor = nullptr;
		ComPtr<IWICFormatConverter> wicFormatConverter = nullptr;
		ComPtr<ID2D1Bitmap> d2dBmpCursor = nullptr;

		Debug::ThrowIfComFailed(
			wicImgFactory->CreateBitmapFromHICON(hCursor, &wicCursor),
			L"创建鼠标图像位图失败"
		);
		Debug::ThrowIfComFailed(
			wicImgFactory->CreateFormatConverter(&wicFormatConverter),
			L"CreateFormatConverter 失败"
		);
		Debug::ThrowIfComFailed(
			wicFormatConverter->Initialize(
				wicCursor.Get(),
				GUID_WICPixelFormat32bppPBGRA,
				WICBitmapDitherTypeNone,
				NULL,
				0.f,
				WICBitmapPaletteTypeMedianCut
			),
			L"IWICFormatConverter 初始化失败"
		);
		Debug::ThrowIfComFailed(
			Env::$instance->GetD2DDC()->CreateBitmapFromWicBitmap(wicFormatConverter.Get(), &d2dBmpCursor),
			L"CreateBitmapFromWicBitmap 失败"
		);

		return d2dBmpCursor;
	}

	ComPtr<ID2D1Bitmap> _MonochromeToD2DBitmap(HBITMAP hbmMask) {
		assert(hbmMask != NULL);

		IWICImagingFactory2* wicImgFactory = Env::$instance->GetWICImageFactory();

		ComPtr<IWICBitmap> wicCursor = nullptr;
		ComPtr<IWICFormatConverter> wicFormatConverter = nullptr;
		ComPtr<ID2D1Bitmap> d2dBmpCursor = nullptr;

		Debug::ThrowIfComFailed(
			wicImgFactory->CreateBitmapFromHBITMAP(hbmMask, NULL, WICBitmapAlphaChannelOption::WICBitmapIgnoreAlpha, &wicCursor),
			L"创建鼠标图像位图失败"
		);
		Debug::ThrowIfComFailed(
			wicImgFactory->CreateFormatConverter(&wicFormatConverter),
			L"CreateFormatConverter 失败"
		);
		Debug::ThrowIfComFailed(
			wicFormatConverter->Initialize(
				wicCursor.Get(),
				GUID_WICPixelFormat32bppPBGRA,
				WICBitmapDitherTypeNone,
				NULL,
				0.f,
				WICBitmapPaletteTypeMedianCut
			),
			L"IWICFormatConverter 初始化失败"
		);
		Debug::ThrowIfComFailed(
			Env::$instance->GetD2DDC()->CreateBitmapFromWicBitmap(wicFormatConverter.Get(), &d2dBmpCursor),
			L"CreateBitmapFromWicBitmap 失败"
		);

		return d2dBmpCursor;
	}

	void _ResolveCursor(HCURSOR hTptCursor, HCURSOR hCursor) {
		assert(hCursor != NULL);

		ICONINFO ii{};
		Debug::ThrowIfWin32Failed(
			GetIconInfo(hCursor, &ii),
			L"GetIconInfo 失败"
		);

		CursorInfo cursorInfo;
		cursorInfo.handle = hCursor;
		cursorInfo.xHotSpot = ii.xHotspot;
		cursorInfo.yHotSpot = ii.yHotspot;
		cursorInfo.isMonochrome = (ii.hbmColor == NULL);

		if (!cursorInfo.isMonochrome) {
			SIZE size = _GetSizeOfHBmp(ii.hbmColor);
			cursorInfo.width = size.cx;
			cursorInfo.height = size.cy;

			cursorInfo.bmp = _CursorToD2DBitmap(hCursor);
		} else {
			SIZE size = _GetSizeOfHBmp(ii.hbmMask);
			cursorInfo.width = size.cx;
			cursorInfo.height = size.cy / 2;

			cursorInfo.bmp = _MonochromeToD2DBitmap(ii.hbmMask);
		}

		if (ii.hbmColor) {
			DeleteBitmap(ii.hbmColor);
		}
		DeleteBitmap(ii.hbmMask);

		_cursorMap[hTptCursor] = cursorInfo;
	}

	SIZE _GetSizeOfHBmp(HBITMAP hBmp) {
		BITMAP bmp{};
		Debug::ThrowIfWin32Failed(
			GetObject(hBmp, sizeof(bmp), &bmp),
			L"GetObject 失败"
		);
		return { bmp.bmWidth, bmp.bmHeight };
	}


	std::map<HCURSOR, CursorInfo> _cursorMap;

	INT _cursorSpeed = 0;

	ComPtr<ID2D1Effect> _monochromeCursorEffect = nullptr;
};
