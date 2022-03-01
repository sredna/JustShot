
/*****************************************************************************\
**                                                                           **
**  JustShot                                                                 **
**                                                                           **
**  Licensed under the GNU General Public License v3.0 (the "License").      **
**  You may not use this file except in compliance with the License.         **
**                                                                           **
**  You can obtain a copy of the License at http://gnu.org/licenses/gpl-3.0  **
**                                                                           **
\*****************************************************************************/

#ifdef _MSC_VER
#pragma warning(disable : 4706 /*assignment within conditional expression*/)
#pragma warning(disable : 4127 /*conditional expression is constant*/)
#endif

#include <Windows.h>
#include <tchar.h>
#include <Shlobj.h>
#include <gdiplus.h>

#define SupportsClipboard() true
#define SupportsTimeout() true
template<class T> void MemFree(T p) { LocalFree((HLOCAL) p); }
template<class T> T MemAlloc(SIZE_T cb) { return (T) LocalAlloc(LMEM_FIXED, cb); }
template<typename T> T ChLwr(T c) { return c >= 'A' && c <= 'Z' ? (T)(c|32) : c; }
static inline bool PathIsAgnosticSeparator(UINT c) { return c == '\\' || c == '/'; }
static inline UINT GetLastErrorAsHResult() { DWORD ec = GetLastError(); return HRESULT_FROM_WIN32(ec); }
static DECLSPEC_NOINLINE FARPROC GetProcAddr(LPCSTR Mod, LPCSTR Nam) { return GetProcAddress(LoadLibraryA(Mod), Nam); }

static HRESULT Write(HANDLE hFile, const void *Data, DWORD cb)
{
	DWORD io;
	BOOL succ = WriteFile(hFile, Data, cb, &io, 0);
	return succ && cb == io ? S_OK : GetLastErrorAsHResult();
}

static HRESULT SHGetSpecialFolderPath(UINT csidl, PWSTR*Out)
{
	TCHAR buf[MAX_PATH];
	BOOL succ = SHGetSpecialFolderPath(0, buf, csidl & ~(CSIDL_FLAG_MASK), false);
	if ((csidl & CSIDL_FLAG_CREATE) && buf[3]) succ = SHGetSpecialFolderPath(0, buf, csidl & ~(CSIDL_FLAG_MASK), true); // Avoid Vista drive root bug
	return succ ? SHStrDup(buf, Out) : E_FAIL;
}

static HRESULT GetEncoderClsidFromExt(PCTSTR Ext, CLSID*pClsid)
{
	UINT n = 0, cb = 0, a = 0, b = 0, succ = 0;
	if (Gdiplus::GetImageEncodersSize(&n, &cb)) return E_FAIL;
	Gdiplus::ImageCodecInfo*pICI = MemAlloc<Gdiplus::ImageCodecInfo*>(cb);
	if (!pICI) return E_OUTOFMEMORY;
	for (UINT i = 0, c = Gdiplus::GetImageEncoders(n, cb, pICI) ? 0 : n; i < c; ++i, a = 0)
	{
		for (PCTSTR p = pICI[i].FilenameExtension, x = Ext; p[a];)
		{
			if (p[a] == '*') ++a;
			if (p[a] == '.') ++a;
			if (x[b] == '.') ++b;
			if (ChLwr(x[b++]) != ChLwr(p[a++]))
			{
				b = 0;
				while(p[a] && p[a] != ';') ++a;
				if (p[a]) a++;
			}
			else if (!x[b])
			{
				*pClsid = pICI[i].Clsid;
				succ++;
				goto done;
			}
		}
	} done:
	MemFree(pICI);
	return succ ? S_OK : E_FAIL;
}

static HRESULT SaveBitmapToBMP24(HDC hDC, HBITMAP hBmp, UINT w, UINT h, PCTSTR Path)
{
	struct { DWORD fh[4]; BITMAPINFO bi; } headers;

	BITMAPINFOHEADER &bih = headers.bi.bmiHeader;
	bih.biSize = 40;
	bih.biWidth = w;
	bih.biHeight = h;
	bih.biPlanes = 1;
	bih.biBitCount = 24;
	bih.biCompression = BI_RGB;
	bih.biSizeImage = 0;
	bih.biXPelsPerMeter = bih.biYPelsPerMeter = 0;
	bih.biClrUsed = bih.biClrImportant = 0;
	UINT stride = 4 * ((w * (bih.biBitCount / 8) + 3) / 4);
	UINT imagedatasize = stride * h, colortablesize = 0;
	headers.fh[3] = 14 + bih.biSize + colortablesize;
	headers.fh[1] = headers.fh[3] + imagedatasize;
	headers.fh[2] = 0;
	headers.fh[0] = UINT(0x4D42) << 16;

	BYTE *bits = MemAlloc<BYTE*>(imagedatasize);
	if (!bits) return E_OUTOFMEMORY;

	HRESULT hr = GetDIBits(hDC, hBmp, 0, h, bits, &headers.bi, DIB_RGB_COLORS) ? S_OK : E_FAIL;
	if (SUCCEEDED(hr))
	{
		HANDLE hFile = CreateFile(Path, GENERIC_WRITE, FILE_SHARE_READ, 0, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL|FILE_FLAG_SEQUENTIAL_SCAN, 0);
		hr = GetLastErrorAsHResult();
		if (hFile != INVALID_HANDLE_VALUE)
		{
			hr = Write(hFile, (char*)&headers + sizeof(headers.fh) - 14, 14 + bih.biSize);
			if (SUCCEEDED(hr)) hr = Write(hFile, bits, imagedatasize);
			CloseHandle(hFile);
		}
	}
	MemFree(bits);
	return hr;
}

static HRESULT SaveBitmapToFile(HDC hDC, HBITMAP hBmp, UINT w, UINT h, PCTSTR Path, CLSID*pEncoderId)
{
	if (pEncoderId)
	{
		Gdiplus::Bitmap bm(hBmp, 0);
		if (bm.GetLastStatus()) return E_FAIL;
		return bm.Save(Path, pEncoderId, 0) ? E_FAIL : S_OK;
	}
	return SaveBitmapToBMP24(hDC, hBmp, w, h, Path);
}

static HRESULT SetClipboardBitmap(HBITMAP hBmp)
{
	SIZE_T succ;
	for (UINT16 t = 0;;) if ((succ = OpenClipboard(0)) || !++t) break;
	EmptyClipboard();
	succ = (SIZE_T) SetClipboardData(CF_BITMAP, hBmp);
	CloseClipboard();
	return succ ? !succ : E_FAIL;
}

static UINT IsClipboardPath(LPCTSTR Path)
{
	return SupportsClipboard() && !lstrcmpi(Path, TEXT("CLIP")) ? 4 : 0;
}

static HRESULT CaptureScreen(CLSID*pEncoderId, PCTSTR Path, BOOL SetClipboard)
{
	HRESULT hr = E_FAIL;
	BOOL succ = false;
	int x = GetSystemMetrics(SM_XVIRTUALSCREEN), y = GetSystemMetrics(SM_YVIRTUALSCREEN);
	UINT smw = SM_CXVIRTUALSCREEN, w = GetSystemMetrics(smw);
	if (sizeof(void*) < 8 && !w) w = GetSystemMetrics(smw = SM_CXSCREEN); // Win95/NT4
	UINT h = GetSystemMetrics(smw + 1);
	HDC hScreenDC = GetDC(0);
	if (hScreenDC)
	{
		HDC hCaptureDC = CreateCompatibleDC(hScreenDC);
		if (hCaptureDC)
		{
			HBITMAP hCaptureBitmap = CreateCompatibleBitmap(hScreenDC, w, h);
			if (hCaptureBitmap)
			{
				HBITMAP hOrgCapBmp = SelectObject(hCaptureDC, hCaptureBitmap);
				succ = BitBlt(hCaptureDC, 0, 0, w, h, hScreenDC, x, y, SRCCOPY|CAPTUREBLT);
				SelectObject(hCaptureDC, hOrgCapBmp);
				if (succ)
				{
					if (SetClipboard)
						hr = SetClipboardBitmap(hCaptureBitmap);
					else
						hr = SaveBitmapToFile(hCaptureDC, hCaptureBitmap, w, h, Path, pEncoderId);
				}
				DeleteObject(hCaptureBitmap);
			}
			DeleteDC(hCaptureDC);
		}
		ReleaseDC(0, hScreenDC);
	}
	return succ ? hr : GetLastErrorAsHResult();
}

template<class T> static inline int App()
{
	using namespace Gdiplus;
	HRESULT hr = S_OK;
	UINT hasPath = false, cch, ec, timeout = 0, setClipboard = 0;
	TCHAR buf[MAX_PATH + 255];
	PCTSTR ext = TEXT("png"), extStart = 0, path = 0;

	BOOL (WINAPI*spdac)(INT_PTR);
	(FARPROC&)spdac = GetProcAddr("USER32", "SetProcessDpiAwarenessContext");
	if (spdac)
	{
		INT_PTR dpi_awareness_context_per_monitor_aware_v2 = INT_PTR(-4);
		spdac(dpi_awareness_context_per_monitor_aware_v2);
	}
	else
	{
		HRESULT (WINAPI*spda)(UINT);
		(FARPROC&)spda = GetProcAddr("SHCORE", "SetProcessDpiAwareness");
		UINT process_per_monitor_dpi_aware = 2;
		if (spda) spda(process_per_monitor_dpi_aware);
	}

	PTSTR cl = GetCommandLine(), p = cl;
	if (*p=='\"') do ++p; while(*p && *p != '\"'); else while(*p > ' ') ++p;
next_param:
	do { if (!*p) break; ++p; } while(*p <= ' ');

	if (SupportsTimeout() && !timeout && p[0] >= '1' && p[0] <= '9' && p[1] <= ' ')
	{
		timeout = (p[0] - '0') * 1000;
		p += 1;
		goto next_param;
	}

	if ((cch = IsClipboardPath(p)))
	{
		setClipboard = ++hasPath;
		path = p, p += cch;
	}

	if (p[0] == '/' && p[1] == '?')
	{
		MessageBoxA(0, "Usage: [1..9] [[[path\\]filename.]extension]", "JustShot v0.4 by Anders Kjersem", MB_ICONINFORMATION);
		return ERROR_CANCELLED;
	}

	ULONG_PTR gdipToken;
	Gdiplus::GdiplusStartupInput gdipStartupInput;
	Gdiplus::Status gdips = Gdiplus::GdiplusStartup(&gdipToken, &gdipStartupInput, NULL);
	if (gdips != Gdiplus::Ok)
	{
		gdipToken = 0;
		ext = TEXT("bmp");
	}

	if (!hasPath && *(path = p))
	{
		for (UINT i = 0; p[i]; ++i)
		{
			if (p[i] == '.' && p[i + 1]) extStart = &p[i + 1];
			if (PathIsAgnosticSeparator(p[i])) extStart = 0, hasPath++;
		}
		if (!extStart && !hasPath) extStart = path;
		if (extStart) ext = extStart;
	}

	if (!hasPath)
	{
		PWSTR olestr = 0;
		// Use the Screenshots folder if it exists or the Pictures folder if not
		HRESULT (WINAPI*shgkfp)(REFKNOWNFOLDERID,DWORD,HANDLE,PWSTR*);
		(FARPROC&)shgkfp = GetProcAddr("SHELL32", "SHGetKnownFolderPath");
		hr = shgkfp ? shgkfp(FOLDERID_Screenshots, KF_FLAG_NO_APPCONTAINER_REDIRECTION|KF_FLAG_CREATE, NULL, &olestr) : E_FAIL;
		if (FAILED(hr)) hr = SHGetSpecialFolderPath(CSIDL_MYPICTURES|CSIDL_FLAG_CREATE, &olestr);

		if (SUCCEEDED(hr))
		{
			BOOL hasName = extStart > path;
			cch = wsprintf(buf, L"%s\\%s", olestr, hasName ? path : TEXT(""));
			SHFree(olestr);
			if (!hasName)
			{
				SYSTEMTIME st;
				GetLocalTime(&st);
				wsprintf(buf + cch, TEXT("%.4u%.2u%.2u_%.2u%.2u%.2u.%s"), st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, ext);
			}
		}
		path = buf;
	}

	if (SUCCEEDED(hr))
	{
		CLSID encoderClsid, *pEncoderId = 0;
		if (gdipToken && SUCCEEDED(hr = GetEncoderClsidFromExt(ext, &encoderClsid)))
		{
			pEncoderId = &encoderClsid;
		}
		else
		{
			hr = lstrcmpi(ext, TEXT("bmp")) ? HRESULT_FROM_WIN32(ERROR_BAD_FORMAT) : S_OK;
		}

		if (SUCCEEDED(hr))
		{
			if (SupportsTimeout() && timeout)
			{
				Sleep(timeout);
				MessageBeep(MB_ICONINFORMATION);
			}
			hr = CaptureScreen(pEncoderId, path, setClipboard);
		}
	}

	if (gdipToken) Gdiplus::GdiplusShutdown(gdipToken);
	return (ec = LOWORD(hr), (HRESULT_FROM_WIN32(ec) == hr)) ? ec : hr;
}

EXTERN_C DECLSPEC_NORETURN void WinMainCRTStartup()
{
	ExitProcess(App<TCHAR>());
}
