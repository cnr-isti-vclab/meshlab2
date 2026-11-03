import ctypes
import os
from pathlib import Path
import sys


_quadwild_root = Path(__file__).resolve().parent / "quadwild-bimdf"
if _quadwild_root.is_dir():
	os.environ.setdefault("MESHLAB2_QUADWILD_ROOT", str(_quadwild_root))

_texture_consolidation_bin = Path(__file__).resolve().parent / "texture-consolidation" / "bin"
if _texture_consolidation_bin.is_dir():
	os.environ.setdefault("MESHLAB2_TEXTURE_CONSOLIDATION_ROOT", str(_texture_consolidation_bin))
	# delvewheel moves the DLLs these programs load into pymeshlab2.libs, under mangled
	# names that no other library uses, and leaves finding them to us: a child process
	# looks for its DLLs in its own directory and then on PATH.
	_vendored_libs = Path(__file__).resolve().parent.parent / "pymeshlab2.libs"
	if os.name == "nt" and _vendored_libs.is_dir():
		os.environ["PATH"] = str(_vendored_libs) + os.pathsep + os.environ.get("PATH", "")

# Geogram's spectral filters load this library by its leaf name at runtime.
if sys.platform == "darwin":
	_geogram_num = Path(__file__).resolve().parent / "libgeogram_num_3rdparty.dylib"
	if _geogram_num.is_file():
		_geogram_num_dll = ctypes.CDLL(str(_geogram_num), mode=ctypes.RTLD_GLOBAL)
elif os.name == "nt":
	_geogram_num = Path(__file__).resolve().parent / "geogram_num_3rdparty.dll"
	if _geogram_num.is_file():
		_geogram_libs = _geogram_num.parent.parent / "pymeshlab2.libs"
		if _geogram_libs.is_dir():
			_geogram_libs_dir = os.add_dll_directory(str(_geogram_libs))
		_geogram_dll_dir = os.add_dll_directory(str(_geogram_num.parent))
		_geogram_num_dll = ctypes.WinDLL(str(_geogram_num))

try:
	from ._meshlab import FilterInfo, FilterRunResult, Mesh, MeshSet
except ModuleNotFoundError:
	# Local development fallback: import extension directly from build directory.
	from _meshlab import FilterInfo, FilterRunResult, Mesh, MeshSet

__all__ = ["Mesh", "MeshSet", "FilterInfo", "FilterRunResult"]
