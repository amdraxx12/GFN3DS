"""Utilitaires Windows en ctypes pur (aucune dependance) : fenetre GeForce NOW, DPI, timer.

Sur les autres systemes, toutes les fonctions existent mais ne font rien (utile pour les tests).
Ce module utilise ses propres instances de DLL (WinDLL) pour ne pas modifier les prototypes
partages avec d'autres bibliotheques (dxcam...).
"""
from __future__ import annotations

import os
import sys
from typing import List, Optional, Tuple

IS_WINDOWS = sys.platform == "win32"
Rect = Tuple[int, int, int, int]  # gauche, haut, droite, bas (pixels physiques)
WindowInfo = Tuple[int, str, Rect, str]  # hwnd, titre, zone cliente, nom de l'exe

if IS_WINDOWS:
    import ctypes
    from ctypes import wintypes

    _user32 = ctypes.WinDLL("user32", use_last_error=True)
    _kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    _winmm = ctypes.WinDLL("winmm")
    _EnumProc = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)

    _user32.EnumWindows.argtypes = [_EnumProc, wintypes.LPARAM]
    _user32.IsWindowVisible.argtypes = [wintypes.HWND]
    _user32.IsIconic.argtypes = [wintypes.HWND]
    _user32.GetWindowTextLengthW.argtypes = [wintypes.HWND]
    _user32.GetWindowTextW.argtypes = [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int]
    _user32.GetClientRect.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.RECT)]
    _user32.ClientToScreen.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.POINT)]
    _user32.ShowWindow.argtypes = [wintypes.HWND, ctypes.c_int]
    _user32.SetForegroundWindow.argtypes = [wintypes.HWND]
    _user32.GetSystemMetrics.argtypes = [ctypes.c_int]
    _user32.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
    _user32.GetWindowThreadProcessId.restype = wintypes.DWORD
    _kernel32.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    _kernel32.OpenProcess.restype = wintypes.HANDLE
    _kernel32.QueryFullProcessImageNameW.argtypes = [
        wintypes.HANDLE, wintypes.DWORD, wintypes.LPWSTR, ctypes.POINTER(wintypes.DWORD)]
    _kernel32.QueryFullProcessImageNameW.restype = wintypes.BOOL
    _kernel32.CloseHandle.argtypes = [wintypes.HANDLE]

    _PROCESS_QUERY_LIMITED_INFORMATION = 0x1000

    def set_dpi_aware() -> None:
        """Coordonnees en pixels physiques (comme dxcam)."""
        try:
            _user32.SetProcessDPIAware()
        except Exception:  # noqa: BLE001
            pass

    def set_timer_resolution(ms: int = 1) -> None:
        try:
            _winmm.timeBeginPeriod(ms)
        except Exception:  # noqa: BLE001
            pass

    def screen_size() -> Tuple[int, int]:
        """Taille de l'ecran principal en pixels."""
        return _user32.GetSystemMetrics(0), _user32.GetSystemMetrics(1)

    def _exe_name(hwnd) -> str:
        try:
            pid = wintypes.DWORD(0)
            _user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
            if not pid.value:
                return ""
            h = _kernel32.OpenProcess(_PROCESS_QUERY_LIMITED_INFORMATION, False, pid.value)
            if not h:
                return ""
            try:
                buf = ctypes.create_unicode_buffer(1024)
                n = wintypes.DWORD(1024)
                if _kernel32.QueryFullProcessImageNameW(h, 0, buf, ctypes.byref(n)):
                    return os.path.basename(buf.value)
                return ""
            finally:
                _kernel32.CloseHandle(h)
        except Exception:  # noqa: BLE001
            return ""

    def list_windows(min_size: int = 64) -> List[WindowInfo]:
        """Fenetres visibles, non reduites, avec un titre et une zone cliente d'au moins min_size px."""
        found: List[WindowInfo] = []

        def cb(hwnd, _lparam):
            try:
                if not _user32.IsWindowVisible(hwnd) or _user32.IsIconic(hwnd):
                    return True
                n = _user32.GetWindowTextLengthW(hwnd)
                if n <= 0:
                    return True
                buf = ctypes.create_unicode_buffer(n + 1)
                _user32.GetWindowTextW(hwnd, buf, n + 1)
                rc = wintypes.RECT()
                if not _user32.GetClientRect(hwnd, ctypes.byref(rc)):
                    return True
                pt = wintypes.POINT(0, 0)
                _user32.ClientToScreen(hwnd, ctypes.byref(pt))
                w, h = rc.right - rc.left, rc.bottom - rc.top
                if w >= min_size and h >= min_size:
                    found.append((int(hwnd or 0), buf.value, (pt.x, pt.y, pt.x + w, pt.y + h),
                                  _exe_name(hwnd)))
            except Exception:  # noqa: BLE001
                pass
            return True

        _user32.EnumWindows(_EnumProc(cb), 0)
        return found

    def bring_to_front(hwnd: int) -> None:
        """Au mieux : Windows peut refuser de donner le focus a un autre processus."""
        try:
            _user32.ShowWindow(hwnd, 9)  # SW_RESTORE
            _user32.SetForegroundWindow(hwnd)
        except Exception:  # noqa: BLE001
            pass

else:

    def set_dpi_aware() -> None:
        pass

    def set_timer_resolution(ms: int = 1) -> None:
        pass

    def screen_size() -> Tuple[int, int]:
        return 0, 0

    def list_windows(min_size: int = 64) -> List[WindowInfo]:
        return []

    def bring_to_front(hwnd: int) -> None:
        pass


GFN_DOWNLOAD_URL = "https://www.nvidia.com/geforce-now/download/"


def _env_dirs(*names: str) -> List[str]:
    out: List[str] = []
    for n in names:
        v = os.environ.get(n, "")
        if v and v not in out:
            out.append(v)
    return out


def gfn_exe_candidates() -> List[str]:
    """Emplacements usuels de GeForceNOW.exe (chemins NON verifies sur une vraie installation)."""
    return [os.path.join(b, "NVIDIA Corporation", "GeForceNOW", "CEF", "GeForceNOW.exe")
            for b in _env_dirs("LOCALAPPDATA", "ProgramFiles", "ProgramFiles(x86)")]


def find_gfn_exe() -> str:
    for p in gfn_exe_candidates():
        if os.path.isfile(p):
            return p
    return ""


def gfn_shortcut_dirs() -> List[Tuple[str, bool]]:
    """(dossier, tout_accepter) ou le client GeForce NOW depose ses raccourcis de jeux.

    Menu Demarrer (NVIDIA Corporation\\Games) : dossier propre a GeForce NOW, tous les raccourcis sont
    des jeux. Bureau : on n'y prend que les .gfnpc (le Bureau contient aussi n'importe quoi d'autre).
    Emplacements d'apres le comportement decrit par des projets tiers, non verifies ici.
    """
    out: List[Tuple[str, bool]] = []
    for base in _env_dirs("APPDATA", "ProgramData"):
        out.append((os.path.join(base, "Microsoft", "Windows", "Start Menu", "Programs",
                                 "NVIDIA Corporation", "Games"), True))
    desk = [os.path.join(b, "Desktop") for b in _env_dirs(
        "USERPROFILE", "OneDrive", "OneDriveConsumer", "OneDriveCommercial", "PUBLIC")]
    out.extend((d, False) for d in desk)
    return out


def find_window(title_part: str, prefer_exe: str = "geforcenow") -> Optional[Tuple[int, Rect]]:
    """(hwnd, zone cliente) de la plus grande fenetre visible dont le titre contient title_part.

    Si plusieurs fenetres correspondent (un onglet de navigateur "GeForce NOW" par exemple),
    celles dont le nom d'executable contient prefer_exe sont retenues en priorite.
    """
    needle = title_part.lower()
    cands = [w for w in list_windows() if needle in w[1].lower()]
    if prefer_exe:
        pref = [w for w in cands if prefer_exe.lower() in w[3].lower()]
        if pref:
            cands = pref
    if not cands:
        return None
    best = max(cands, key=lambda w: (w[2][2] - w[2][0]) * (w[2][3] - w[2][1]))
    return best[0], best[2]
