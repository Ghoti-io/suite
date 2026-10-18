#!/usr/bin/env python3
"""Install checksum-pinned wasi-sdk 34 and Wasmtime 49.0.3 C API tools."""
from __future__ import annotations

import hashlib
import io
import os
import pathlib
import platform
import shutil
import subprocess
import tarfile
import urllib.request


ASSETS = {
    "x86_64": {
        "wasi": ("wasi-sdk-34.0-x86_64-linux.tar.gz",
                 "b761e3a0721dbae9c09a0059e5fdb2bf917d1b4a8a7b430fb3b5aafb0984b2c4"),
        "wasmtime": ("wasmtime-v49.0.3-x86_64-linux.tar.xz",
                     "4001225413f208345b980525f31c4080e1873808b38d0ddc7918497340667fbb"),
        "wasmtime-c-api": ("wasmtime-v49.0.3-x86_64-linux-c-api.tar.xz",
                           "49c9641ac1f6158ed90404115315b36a9ae68338acda16fcf00f0819622ccd97"),
    },
    "aarch64": {
        "wasi": ("wasi-sdk-34.0-arm64-linux.tar.gz",
                 "f7e243dff54d60bcc576e94d6166b69f410f2500ae4a9ceef34315be10e77971"),
        "wasmtime": ("wasmtime-v49.0.3-aarch64-linux.tar.xz",
                     "880fcb026da88ad7fc1713d572b0d03d139758b7da96cd3cb8c9cbf847a521df"),
        "wasmtime-c-api": ("wasmtime-v49.0.3-aarch64-linux-c-api.tar.xz",
                           "fb9c1a101c092cd3b4536ddb4c68ae31ad70d78bbdbc408d0e007f05a2fdd409"),
    },
}


def download(name: str, expected: str, url: str) -> bytes:
    request = urllib.request.Request(url, headers={"User-Agent": "ghoti-lang-wasm-corpus/1"})
    with urllib.request.urlopen(request, timeout=180) as response:
        data = response.read()
    actual = hashlib.sha256(data).hexdigest()
    if actual != expected:
        raise SystemExit(f"{name}: SHA-256 mismatch: expected {expected}, got {actual}")
    return data


def extract(data: bytes, destination: pathlib.Path) -> None:
    destination.mkdir(parents=True, exist_ok=True)
    with tarfile.open(fileobj=io.BytesIO(data), mode="r:*") as archive:
        archive.extractall(destination, filter="data")


def verify_clang_against_archive(data: bytes, package: pathlib.Path) -> tuple[str, str, str]:
    """Prove the installed clang bytes came from the checksum-verified tarball."""
    link = package / "bin/clang"
    resolved = link.resolve(strict=True)
    package_real = package.resolve(strict=True)
    try:
        relative = resolved.relative_to(package_real)
    except ValueError as error:
        raise SystemExit("wasi-sdk clang resolves outside the extracted SDK") from error
    member_name = f"{package.name}/{relative.as_posix()}"
    with tarfile.open(fileobj=io.BytesIO(data), mode="r:*") as archive:
        try:
            member = archive.getmember(member_name)
        except KeyError as error:
            raise SystemExit(f"wasi-sdk archive is missing clang target {member_name}") from error
        if not member.isfile():
            raise SystemExit(f"wasi-sdk clang target is not a regular file: {member_name}")
        archived = archive.extractfile(member)
        if archived is None:
            raise SystemExit(f"cannot read wasi-sdk clang target {member_name}")
        archive_bytes = archived.read()
    installed_bytes = resolved.read_bytes()
    archived_hash = hashlib.sha256(archive_bytes).hexdigest()
    installed_hash = hashlib.sha256(installed_bytes).hexdigest()
    if installed_hash != archived_hash:
        raise SystemExit("installed wasi-sdk clang differs from the verified archive member")
    version_result = subprocess.run([str(link), "--version"], check=True,
        text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    version = version_result.stdout.splitlines()[0] if version_result.stdout else ""
    target_result = subprocess.run([str(link), "-dumpmachine"], check=True,
        text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    target = target_result.stdout.strip()
    if not version.startswith("clang version ") or target != "wasm32-unknown-wasip1":
        raise SystemExit(f"unexpected wasi-sdk clang identity: {version!r}, target {target!r}")
    return installed_hash, version, target


def tree_digest(root: pathlib.Path) -> str:
    """Hash every path and file byte under an SDK link-input directory."""
    if not root.is_dir():
        raise SystemExit(f"required wasi-sdk link-input directory is missing: {root}")
    root_real = root.resolve(strict=True)
    digest = hashlib.sha256()
    for path in sorted(root.rglob("*"), key=lambda item: item.relative_to(root).as_posix()):
        relative = path.relative_to(root).as_posix()
        if path.is_symlink():
            try:
                target = path.resolve(strict=True)
                target_relative = target.relative_to(root_real).as_posix()
            except (OSError, RuntimeError, ValueError) as error:
                raise SystemExit(
                    f"SDK link-input symlink is dangling, cyclic, or escapes its measured tree: {path}") from error
            record = f"L\0{relative}\0{path.readlink()}\0{target_relative}\n".encode()
        elif path.is_dir():
            record = f"D\0{relative}\n".encode()
        elif path.is_file():
            content_hash = hashlib.sha256(path.read_bytes()).hexdigest()
            record = f"F\0{relative}\0{content_hash}\n".encode()
        else:
            raise SystemExit(f"unsupported file in wasi-sdk link inputs: {path}")
        digest.update(record)
    return digest.hexdigest()


def verify_link_inputs(package: pathlib.Path) -> tuple[str, str, str, str, str, str, str]:
    """Identify and hash linker, WASI sysroot, and compiler runtime inputs."""
    linker = package / "bin/wasm-ld"
    try:
        resolved_linker = linker.resolve(strict=True)
        linker_relative = resolved_linker.relative_to(package.resolve(strict=True))
    except (OSError, ValueError) as error:
        raise SystemExit(f"wasi-sdk wasm-ld is missing or resolves outside its SDK: {error}")
    linker_hash = hashlib.sha256(resolved_linker.read_bytes()).hexdigest()
    linker_output = subprocess.run([str(linker), "--version"], check=True,
        text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT).stdout.splitlines()
    linker_version = linker_output[0] if linker_output else ""
    if not linker_version.startswith("LLD "):
        raise SystemExit(f"unexpected wasi-sdk wasm-ld identity: {linker_version!r}")

    # wasi-sdk's clang driver returns an error for -print-sysroot. Verify the
    # documented archive layout directly instead of treating that unsupported
    # driver query as a failed SDK installation.
    sysroot = package / "share/wasi-sysroot"
    clang = package / "bin/clang"
    version = subprocess.run([str(clang), "--version"], check=True,
        text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT).stdout.splitlines()[0]
    version_fields = version.split()
    if len(version_fields) < 3 or version_fields[0] != "clang" or version_fields[1] != "version":
        raise SystemExit(f"cannot derive wasi-sdk clang resource version: {version!r}")
    major = version_fields[2].split(".", 1)[0]
    resource_root = package / "lib/clang"
    resources = [path for path in resource_root.iterdir()
                 if path.is_dir() and path.name.split(".", 1)[0] == major]
    if len(resources) != 1:
        raise SystemExit(
            f"expected one wasi-sdk clang {major} resource directory under {resource_root}, "
            f"found {len(resources)}")
    resource = resources[0]
    try:
        sysroot = sysroot.resolve(strict=True)
        resource = resource.resolve(strict=True)
        sysroot_relative = sysroot.relative_to(package.resolve(strict=True))
        resource_relative = resource.relative_to(package.resolve(strict=True))
    except (OSError, ValueError) as error:
        raise SystemExit(f"wasi-sdk compiler link inputs resolve outside its SDK: {error}")
    if sysroot != (package / "share/wasi-sysroot").resolve(strict=True):
        raise SystemExit(f"unexpected wasi-sdk sysroot: {sysroot}")
    if not resource_relative.parts or resource_relative.parts[0] != "lib":
        raise SystemExit(f"unexpected wasi-sdk compiler resource directory: {resource}")
    return (linker_relative.as_posix(), linker_hash, linker_version,
            sysroot_relative.as_posix(), tree_digest(sysroot),
            resource_relative.as_posix(), tree_digest(resource))


def native_gcc_tool(driver: str, name: str) -> pathlib.Path:
    """Resolve the executable GCC will select, including PATH and symlinks."""
    selected = driver if name == "driver" else subprocess.run(
        [driver, f"-print-prog-name={name}"], check=True, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT).stdout.strip()
    if not selected:
        raise SystemExit(f"GCC returned no {name} executable")
    located = selected if "/" in selected else shutil.which(selected)
    if not located:
        raise SystemExit(f"GCC {name} executable is missing: {selected}")
    resolved = pathlib.Path(located).resolve(strict=True)
    if not resolved.is_file() or not os.access(resolved, os.X_OK):
        raise SystemExit(f"GCC {name} executable is missing or not executable: {resolved}")
    return resolved


def record_native_gcc_tools(marker_root: pathlib.Path) -> None:
    driver = shutil.which("gcc")
    if not driver:
        raise SystemExit("native GCC driver is missing")
    for name in ("driver", "cc1", "as", "collect2", "ld"):
        executable = native_gcc_tool(driver, name)
        (marker_root / f"native-gcc-{name}-path").write_text(str(executable) + "\n")
        (marker_root / f"native-gcc-{name}.sha256").write_text(
            hashlib.sha256(executable.read_bytes()).hexdigest() + "\n")


def main() -> None:
    arch = platform.machine()
    if arch not in ASSETS:
        raise SystemExit(f"unsupported architecture for pinned test tools: {arch}")
    assets = ASSETS[arch]
    work = pathlib.Path("/tmp/lang-wasm-corpus-tools")
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir()
    wasi_name, wasi_hash = assets["wasi"]
    wasi_url = f"https://github.com/WebAssembly/wasi-sdk/releases/download/wasi-sdk-34/{wasi_name}"
    wasi_data = download("wasi-sdk 34", wasi_hash, wasi_url)
    extract(wasi_data, work / "wasi")
    wasi_dirs = list((work / "wasi").glob("wasi-sdk-34.0*"))
    if len(wasi_dirs) != 1 or not (wasi_dirs[0] / "bin/clang").is_file():
        raise SystemExit("wasi-sdk archive has an unexpected layout")
    clang_hash, clang_version, clang_target = verify_clang_against_archive(
        wasi_data, wasi_dirs[0])
    (linker_path, linker_hash, linker_version, sysroot_path, sysroot_hash,
     resource_path, resource_hash) = verify_link_inputs(wasi_dirs[0])
    final_wasi = pathlib.Path("/opt/wasi-sdk-34.0")
    shutil.rmtree(final_wasi, ignore_errors=True)
    shutil.move(str(wasi_dirs[0]), final_wasi)

    engine_name, engine_hash = assets["wasmtime"]
    engine_url = f"https://github.com/bytecodealliance/wasmtime/releases/download/v49.0.3/{engine_name}"
    engine_data = download("Wasmtime 49.0.3", engine_hash, engine_url)
    extract(engine_data, work / "wasmtime")
    engines = list((work / "wasmtime").rglob("wasmtime"))
    if len(engines) != 1 or not engines[0].is_file():
        raise SystemExit("Wasmtime archive has an unexpected layout")
    destination = pathlib.Path("/usr/local/bin/wasmtime")
    shutil.copyfile(engines[0], destination)
    destination.chmod(0o755)
    engine_binary_hash = hashlib.sha256(destination.read_bytes()).hexdigest()

    api_name, api_hash = assets["wasmtime-c-api"]
    api_url = f"https://github.com/bytecodealliance/wasmtime/releases/download/v49.0.3/{api_name}"
    api_data = download("Wasmtime 49.0.3 C API", api_hash, api_url)
    extract(api_data, work / "c-api")
    api_package = work / "c-api" / api_name.removesuffix(".tar.xz")
    if (not (api_package / "include/wasmtime.h").is_file() or
            not (api_package / "include/wasm.h").is_file() or
            not (api_package / "lib/libwasmtime.so").is_file()):
        raise SystemExit("Wasmtime C API archive has an unexpected layout")
    api_headers_hash = tree_digest(api_package / "include")
    api_library_hash = hashlib.sha256(
        (api_package / "lib/libwasmtime.so").read_bytes()).hexdigest()
    final_api = pathlib.Path("/opt/wasmtime-c-api-49.0.3")
    shutil.rmtree(final_api, ignore_errors=True)
    shutil.move(str(api_package), final_api)
    marker_root = pathlib.Path("/usr/local/share/ghoti-lang-wasm-corpus")
    marker_root.mkdir(parents=True, exist_ok=True)
    record_native_gcc_tools(marker_root)
    (final_wasi / ".ghoti-archive-sha256").write_text(wasi_hash + "\n")
    (final_wasi / ".ghoti-clang-archive-sha256").write_text(wasi_hash + "\n")
    (final_wasi / ".ghoti-clang-binary-sha256").write_text(clang_hash + "\n")
    (final_wasi / ".ghoti-clang-version").write_text(clang_version + "\n")
    (final_wasi / ".ghoti-clang-target").write_text(clang_target + "\n")
    (final_wasi / ".ghoti-wasm-ld-path").write_text(linker_path + "\n")
    (final_wasi / ".ghoti-wasm-ld-binary-sha256").write_text(linker_hash + "\n")
    (final_wasi / ".ghoti-wasm-ld-version").write_text(linker_version + "\n")
    (final_wasi / ".ghoti-wasi-sysroot-path").write_text(sysroot_path + "\n")
    (final_wasi / ".ghoti-wasi-sysroot-sha256").write_text(sysroot_hash + "\n")
    (final_wasi / ".ghoti-clang-resource-path").write_text(resource_path + "\n")
    (final_wasi / ".ghoti-clang-resource-sha256").write_text(resource_hash + "\n")
    (marker_root / "wasmtime-archive.sha256").write_text(engine_hash + "\n")
    (marker_root / "wasmtime-binary.sha256").write_text(engine_binary_hash + "\n")
    (final_api / ".ghoti-archive-sha256").write_text(api_hash + "\n")
    (final_api / ".ghoti-headers-sha256").write_text(api_headers_hash + "\n")
    (final_api / ".ghoti-library-sha256").write_text(api_library_hash + "\n")
    print(f"installed wasi-sdk 34 ({clang_version}; {clang_target}), "
          f"Wasmtime 49.0.3 CLI and C API for {arch}")


if __name__ == "__main__":
    main()
