#!/usr/bin/env python3
"""
Fix DuckDB extension footer broken by append_metadata.cmake.

Upstream DuckDB v1.5.2 的 scripts/append_metadata.cmake 用
`file(READ "${NULL_FILE}" EMPTY_BYTE)` 读单字节 0,但 cmake string 不能含 \\0,
EMPTY_BYTE 实际是空串,后续 `string(REPEAT "${EMPTY_BYTE}" 32 EMPTY_32)` 也是空,
导致 METADATAn padding 全部缺失。

DuckDB extension loader (extension_load.cpp:252) 从文件末尾读 FOOTER_SIZE=512 字节,
后 256 字节是签名 blob,前 256 字节是 8 个 32-byte METADATA(file 内逆序排列):
  - metadata_field[0] = file 末 32 byte = magic_value (期望 "4" + 31 NUL)
  - metadata_field[1] = 平台 (linux_arm64 + NUL pad)
  - metadata_field[2] = duckdb 版本 (v1.5.2)
  - metadata_field[3] = extension_version ("")
  - metadata_field[4] = abi_type ("CPP")
  - metadata_field[5..7] = 未用

破损 extension 末尾会看到 `linux_arm644`(linux_arm64 + 后接 META1 "4" 没 padding)。
此脚本定位 "duckdb_signature" custom section,从那之后重写正确 512-byte footer。
"""
import sys

SIG_MARKER = b"duckdb_signature"
SIG_MARKER_LEN = len(SIG_MARKER)            # 16
CUSTOM_SECTION_LEN_BYTES = 2                # `\x80 \x04` after marker
FIELD_SIZE = 32
N_FIELDS = 8
SIG_BLOB_SIZE = 256
FOOTER_SIZE = FIELD_SIZE * N_FIELDS + SIG_BLOB_SIZE  # 512


def pad32(s):
    b = s.encode() if isinstance(s, str) else s
    if len(b) > FIELD_SIZE:
        raise SystemExit(f"field too long ({len(b)} > {FIELD_SIZE}): {b!r}")
    return b.ljust(FIELD_SIZE, b'\0')


def fix_footer(path, platform, duckdb_version="v1.5.2", abi="CPP", extension_version=""):
    with open(path, 'rb') as f:
        data = f.read()

    sig_idx = data.rfind(SIG_MARKER)
    if sig_idx < 0:
        raise SystemExit(f"no duckdb_signature marker in {path}")

    keep_until = sig_idx + SIG_MARKER_LEN + CUSTOM_SECTION_LEN_BYTES

    # cmake 里 backward append:METADATA8 first, METADATA1 last。文件内顺序同。
    # DuckDB 读 file_end - FOOTER_SIZE 起 8 × 32:
    #   metadata_field[i] = bytes[FOOTER_SIZE - SIGNATURE_SIZE + i*32 : ... + 32]
    #   reverse(metadata_field):  metadata_field[0] = 文件末尾那一个 32-byte = METADATA1
    # 所以文件内 metadata bytes 顺序 (从 keep_until 开始) 是:
    #   META8, META7, META6, META5, META4, META3, META2, META1, signature_blob
    empty_field = b'\0' * FIELD_SIZE
    fields_in_file_order = [
        empty_field,                           # META8 unused
        empty_field,                           # META7 unused
        empty_field,                           # META6 unused
        pad32(abi),                            # META5 ABI_TYPE
        pad32(extension_version),              # META4 EXTENSION_VERSION
        pad32(duckdb_version),                 # META3 VERSION_FIELD
        pad32(platform),                       # META2 PLATFORM
        pad32("4"),                            # META1 magic = "4"
    ]
    metadata_bytes = b''.join(fields_in_file_order)
    if len(metadata_bytes) != FIELD_SIZE * N_FIELDS:
        raise RuntimeError(f"metadata size {len(metadata_bytes)} != {FIELD_SIZE * N_FIELDS}")
    new_footer = metadata_bytes + b'\0' * SIG_BLOB_SIZE
    if len(new_footer) != FOOTER_SIZE:
        raise RuntimeError(f"footer size {len(new_footer)} != {FOOTER_SIZE}")

    new_data = data[:keep_until] + new_footer
    with open(path, 'wb') as f:
        f.write(new_data)
    print(f"patched {path}: total_size={len(new_data)} platform={platform!r} duckdb_version={duckdb_version!r}")


if __name__ == "__main__":
    if len(sys.argv) < 3:
        print(f"usage: {sys.argv[0]} <extension_path> <platform> [duckdb_version] [abi] [extension_version]")
        sys.exit(1)
    fix_footer(
        sys.argv[1],
        sys.argv[2],
        duckdb_version=sys.argv[3] if len(sys.argv) > 3 else "v1.5.2",
        abi=sys.argv[4] if len(sys.argv) > 4 else "CPP",
        extension_version=sys.argv[5] if len(sys.argv) > 5 else "",
    )
