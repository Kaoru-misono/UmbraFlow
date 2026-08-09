"""Small filesystem boundary used by authoring, replay, and handoff code."""

from __future__ import annotations

import os
import stat
from pathlib import Path, PurePosixPath
from typing import Iterator


_REPARSE_POINT = getattr(stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0x400)


class UnsafePath(RuntimeError):
    pass


def is_reparse(metadata: os.stat_result) -> bool:
    return stat.S_ISLNK(metadata.st_mode) or bool(
        getattr(metadata, "st_file_attributes", 0) & _REPARSE_POINT
    )


def identity(metadata: os.stat_result) -> tuple[int, int, int, int, int]:
    return (
        metadata.st_dev,
        metadata.st_ino,
        stat.S_IFMT(metadata.st_mode),
        metadata.st_nlink,
        getattr(metadata, "st_file_attributes", 0),
    )


def container_identity(metadata: os.stat_result) -> tuple[int, int, int, int]:
    """identity() minus the one field a directory changes by losing its own entries.

    POSIX counts one link per subdirectory, so removing a child directory drops the
    parent's st_nlink. Comparing the full identity across a delete would therefore
    refuse every tree deeper than one level, on the strength of a change the delete
    itself caused. Windows reports 1 for every directory, so st_nlink separates
    nothing there either. Use this only where entries are being removed; anywhere
    else identity() is the stricter and correct comparison.
    """

    return (
        metadata.st_dev,
        metadata.st_ino,
        stat.S_IFMT(metadata.st_mode),
        getattr(metadata, "st_file_attributes", 0),
    )


def require_plain_ancestors(path: Path | str, *, include_leaf: bool = False) -> None:
    """Reject links, junctions, and reparse points at every existing path level."""

    target = Path(path).absolute()
    chain = list(reversed(target.parents))
    if include_leaf:
        chain.append(target)
    for component in chain:
        if not os.path.lexists(component):
            continue
        try:
            metadata = component.lstat()
        except OSError as error:
            raise UnsafePath(f"path component is unavailable: {component}") from error
        if not stat.S_ISDIR(metadata.st_mode) or is_reparse(metadata):
            raise UnsafePath(f"path hierarchy contains a non-plain directory: {component}")


def require_plain_directory(path: Path | str) -> os.stat_result:
    target = Path(path).absolute()
    require_plain_ancestors(target)
    try:
        metadata = target.lstat()
    except OSError as error:
        raise UnsafePath(f"required directory is unavailable: {target}") from error
    if not stat.S_ISDIR(metadata.st_mode) or is_reparse(metadata):
        raise UnsafePath(f"required path is not a plain directory: {target}")
    return metadata


def make_plain_directories(path: Path | str) -> None:
    target = Path(path).absolute()
    require_plain_ancestors(target)
    missing: list[Path] = []
    cursor = target
    while not os.path.lexists(cursor):
        missing.append(cursor)
        cursor = cursor.parent
    require_plain_directory(cursor)
    for directory in reversed(missing):
        directory.mkdir()
        require_plain_directory(directory)
    require_plain_directory(target)


def require_plain_file(path: Path | str) -> os.stat_result:
    target = Path(path).absolute()
    require_plain_ancestors(target)
    try:
        metadata = target.lstat()
    except OSError as error:
        raise UnsafePath(f"required file is unavailable: {target}") from error
    if not stat.S_ISREG(metadata.st_mode) or is_reparse(metadata) or metadata.st_nlink != 1:
        raise UnsafePath(f"required path is not one plain, unaliased file: {target}")
    return metadata


def open_plain_read(path: Path | str) -> int:
    target = Path(path).absolute()
    before = require_plain_file(target)
    flags = os.O_RDONLY | getattr(os, "O_BINARY", 0) | getattr(os, "O_NOFOLLOW", 0)
    try:
        descriptor = os.open(target, flags)
    except OSError as error:
        raise UnsafePath(f"plain file cannot be opened: {target}") from error
    try:
        opened = os.fstat(descriptor)
        after = target.lstat()
        if (
            not stat.S_ISREG(opened.st_mode)
            or opened.st_nlink != 1
            or is_reparse(after)
            or identity(before) != identity(opened)
            or identity(opened) != identity(after)
        ):
            raise UnsafePath(f"plain file identity changed while opening: {target}")
        return descriptor
    except BaseException:
        os.close(descriptor)
        raise


def read_descriptor(descriptor: int, *, maximum: int | None = None) -> bytes:
    os.lseek(descriptor, 0, os.SEEK_SET)
    chunks: list[bytes] = []
    size = 0
    while True:
        chunk = os.read(descriptor, 64 * 1024)
        if not chunk:
            break
        size += len(chunk)
        if maximum is not None and size > maximum:
            raise UnsafePath("plain file exceeds its permitted size")
        chunks.append(chunk)
    os.lseek(descriptor, 0, os.SEEK_SET)
    return b"".join(chunks)


def read_plain_file(path: Path | str, *, maximum: int | None = None) -> bytes:
    target = Path(path).absolute()
    descriptor = open_plain_read(target)
    try:
        before = os.fstat(descriptor)
        content = read_descriptor(descriptor, maximum=maximum)
        opened = os.fstat(descriptor)
        current = target.lstat()
        if identity(before) != identity(opened) or identity(opened) != identity(current):
            raise UnsafePath(f"plain file identity changed while reading: {target}")
        if opened.st_size != len(content):
            raise UnsafePath(f"plain file size changed while reading: {target}")
        return content
    finally:
        os.close(descriptor)


def write_new_file(path: Path | str, content: bytes, *, mode: int = 0o600) -> None:
    target = Path(path).absolute()
    require_plain_directory(target.parent)
    flags = (
        os.O_WRONLY
        | os.O_CREAT
        | os.O_EXCL
        | getattr(os, "O_BINARY", 0)
        | getattr(os, "O_NOFOLLOW", 0)
    )
    descriptor = os.open(target, flags, mode)
    try:
        view = memoryview(content)
        while view:
            written = os.write(descriptor, view)
            view = view[written:]
        os.fsync(descriptor)
        opened = os.fstat(descriptor)
        if not stat.S_ISREG(opened.st_mode) or opened.st_nlink != 1:
            raise UnsafePath(f"new file is not plain and unaliased: {target}")
    finally:
        os.close(descriptor)
    require_plain_file(target)


def confined_relative(raw: object, *, prefix: str | None = None) -> PurePosixPath:
    if not isinstance(raw, str) or not raw or "\\" in raw or "\0" in raw:
        raise UnsafePath(f"unsafe relative path {raw!r}")
    if raw.startswith("/") or (len(raw) >= 2 and raw[1] == ":"):
        raise UnsafePath(f"unsafe relative path {raw!r}")
    parts = raw.split("/")
    if any(part in {"", ".", ".."} for part in parts):
        raise UnsafePath(f"unsafe relative path {raw!r}")
    if prefix is not None and (not parts or parts[0] != prefix):
        raise UnsafePath(f"path must be confined below {prefix}/: {raw!r}")
    return PurePosixPath(*parts)


def walk_plain_files(root: Path | str) -> set[str]:
    base = Path(root).absolute()
    files: set[str] = set()

    def visit(directory: Path, relative: PurePosixPath) -> None:
        directory_before = require_plain_directory(directory)
        with os.scandir(directory) as iterator:
            entries = list(iterator)
        for entry in entries:
            child = Path(entry.path)
            metadata = child.lstat()
            current = child.lstat()
            child_relative = relative / entry.name
            if identity(metadata) != identity(current) or is_reparse(current):
                raise UnsafePath(f"path identity changed or is a reparse point: {child_relative}")
            if stat.S_ISDIR(current.st_mode):
                visit(child, child_relative)
            elif stat.S_ISREG(current.st_mode) and current.st_nlink == 1:
                files.add(child_relative.as_posix())
            else:
                raise UnsafePath(f"path is not one plain file: {child_relative}")
        if identity(directory_before) != identity(directory.lstat()):
            raise UnsafePath(f"directory identity changed while walking: {directory}")

    visit(base, PurePosixPath())
    return files


def remove_plain_tree(root: Path | str) -> None:
    """Delete one verified tree; never follow or silently unlink a reparse point."""

    target = Path(root).absolute()
    root_identity = container_identity(require_plain_directory(target))
    with os.scandir(target) as iterator:
        entries = list(iterator)
    for entry in entries:
        child = Path(entry.path)
        scanned = child.lstat()
        current = child.lstat()
        if identity(scanned) != identity(current) or is_reparse(current):
            raise UnsafePath(f"refusing to delete changed/reparse path: {child}")
        if stat.S_ISDIR(current.st_mode):
            remove_plain_tree(child)
        elif stat.S_ISREG(current.st_mode) and current.st_nlink == 1:
            if identity(current) != identity(child.lstat()):
                raise UnsafePath(f"file identity changed before delete: {child}")
            child.unlink()
        else:
            raise UnsafePath(f"refusing to delete non-plain path: {child}")
    # A concurrent entry appearing under target is still refused: rmdir fails with
    # ENOTEMPTY rather than deleting anything the scan above did not verify.
    if root_identity != container_identity(target.lstat()):
        raise UnsafePath(f"directory identity changed before delete: {target}")
    target.rmdir()


def existing_entries(directory: Path | str) -> Iterator[Path]:
    root = Path(directory).absolute()
    require_plain_directory(root)
    with os.scandir(root) as iterator:
        entries = list(iterator)
    for entry in entries:
        child = Path(entry.path)
        scanned = child.lstat()
        current = child.lstat()
        if identity(scanned) != identity(current) or is_reparse(current):
            raise UnsafePath(f"directory entry changed or is a reparse point: {child}")
        yield child


def paths_overlap(left: Path | str, right: Path | str) -> bool:
    a = Path(left).absolute().resolve(strict=False)
    b = Path(right).absolute().resolve(strict=False)
    try:
        common = Path(os.path.commonpath((a, b)))
    except ValueError:
        return False
    return common == a or common == b
