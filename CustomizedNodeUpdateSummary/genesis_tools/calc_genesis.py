#!/usr/bin/env python3
"""calc_genesis.py

用 Python 复现 Bitcoin Core (v28.x) `CreateGenesisBlock()` 的 genesis 构造方式，并挖出满足 PoW 的 nonce。

- 构造 coinbase 交易：
  scriptSig = CScript() << 486604799 << CScriptNum(4) << <timestamp bytes>
  vout[0].scriptPubKey = <P2PK pubkey> OP_CHECKSIG

- merkle root（仅 1 笔交易）== txid
- block hash = sha256d(block_header)

默认只遍历 nonce；如果你想扩大搜索空间，可改 timestamp/time。

用法示例：
  python3 calc_genesis.py --timestamp "..." --time 1700000000 --bits 0x207fffff --pubkeyhex <hex>
"""

from __future__ import annotations

import argparse
import hashlib
import struct
from typing import Tuple


def sha256d(b: bytes) -> bytes:
    return hashlib.sha256(hashlib.sha256(b).digest()).digest()


def varint(n: int) -> bytes:
    if n < 0:
        raise ValueError("varint negative")
    if n < 0xFD:
        return bytes([n])
    if n <= 0xFFFF:
        return b"\xFD" + struct.pack("<H", n)
    if n <= 0xFFFFFFFF:
        return b"\xFE" + struct.pack("<I", n)
    return b"\xFF" + struct.pack("<Q", n)


def serialize_scriptnum(value: int) -> bytes:
    # 与 Bitcoin Core 的 CScriptNum::serialize 等价（非负数即可满足这里使用）
    if value == 0:
        return b""
    if value < 0:
        raise ValueError("negative not supported here")
    out = bytearray()
    v = value
    while v:
        out.append(v & 0xFF)
        v >>= 8
    # 符号位处理（这里 value 为正数）
    if out[-1] & 0x80:
        out.append(0x00)
    return bytes(out)


def script_push(data: bytes) -> bytes:
    l = len(data)
    if l < 0x4C:
        return bytes([l]) + data
    if l <= 0xFF:
        return b"\x4c" + bytes([l]) + data
    if l <= 0xFFFF:
        return b"\x4d" + struct.pack("<H", l) + data
    return b"\x4e" + struct.pack("<I", l) + data


def bits_to_target(bits: int) -> int:
    exp = (bits >> 24) & 0xFF
    mant = bits & 0x007FFFFF
    if bits & 0x00800000:
        raise ValueError("negative compact")
    if exp <= 3:
        return mant >> (8 * (3 - exp))
    return mant << (8 * (exp - 3))


def build_genesis_tx(timestamp: str, pubkeyhex: str, reward_sats: int) -> bytes:
    ts_bytes = timestamp.encode("utf-8")

    # scriptSig: push(486604799) + push(CScriptNum(4)) + push(timestamp_bytes)
    script_sig = b"".join(
        [
            script_push(serialize_scriptnum(486604799)),
            script_push(serialize_scriptnum(4)),
            script_push(ts_bytes),
        ]
    )

    # tx format (legacy):
    # version | vin | vout | locktime
    version = struct.pack("<I", 1)

    # vin[0]
    prevout_hash = b"\x00" * 32
    prevout_n = struct.pack("<I", 0xFFFFFFFF)
    script_len = varint(len(script_sig))
    sequence = struct.pack("<I", 0xFFFFFFFF)
    vin = (
        varint(1)
        + prevout_hash
        + prevout_n
        + script_len
        + script_sig
        + sequence
    )

    # vout[0]
    pubkey = bytes.fromhex(pubkeyhex)
    if len(pubkey) != 65 or pubkey[0] != 0x04:
        raise ValueError("pubkeyhex 应为 65 字节未压缩公钥（以 04 开头）")
    script_pubkey = bytes([0x41]) + pubkey + bytes([0xAC])  # OP_CHECKSIG

    vout = (
        varint(1)
        + struct.pack("<Q", reward_sats)
        + varint(len(script_pubkey))
        + script_pubkey
    )

    locktime = struct.pack("<I", 0)
    tx = version + vin + vout + locktime
    return tx


def txid_le(tx: bytes) -> bytes:
    return sha256d(tx)


def merkle_root_le(single_txid_le: bytes) -> bytes:
    # genesis 只有 1 笔 tx
    return single_txid_le


def header_bytes(version: int, merkle_le: bytes, ntime: int, nbits: int, nonce: int) -> bytes:
    return (
        struct.pack("<I", version)
        + (b"\x00" * 32)  # prevhash
        + merkle_le
        + struct.pack("<I", ntime)
        + struct.pack("<I", nbits)
        + struct.pack("<I", nonce)
    )


def mine_genesis(
    *,
    timestamp: str,
    pubkeyhex: str,
    ntime: int,
    nbits: int,
    version: int,
    reward_sats: int,
    start_nonce: int,
    max_nonce: int,
) -> Tuple[int, str, str]:
    tx = build_genesis_tx(timestamp, pubkeyhex, reward_sats)
    txid = txid_le(tx)
    mr_le = merkle_root_le(txid)

    target = bits_to_target(nbits)

    for nonce in range(start_nonce, max_nonce + 1):
        hdr = header_bytes(version, mr_le, ntime, nbits, nonce)
        h_le = sha256d(hdr)
        h_int = int.from_bytes(h_le, "little")
        if h_int <= target:
            genesis_hash_be = h_le[::-1].hex()
            merkle_root_be = mr_le[::-1].hex()
            return nonce, genesis_hash_be, merkle_root_be

    raise RuntimeError("在给定 nonce 范围内未找到满足 PoW 的 genesis；请扩大范围或调低难度(bits)")


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description="计算/挖出 Bitcoin Core 风格 genesis（v27.x CreateGenesisBlock 兼容）")
    p.add_argument("--timestamp", required=True, help="genesis coinbase timestamp 文本")
    p.add_argument("--pubkeyhex", required=True, help="65字节未压缩公钥 hex（以 04 开头）")
    p.add_argument("--time", type=int, required=True, help="nTime (uint32)")
    p.add_argument("--bits", required=True, help="nBits，形如 0x1d00ffff 或 0x207fffff")
    p.add_argument("--version", type=int, default=1, help="block version (默认 1)")
    p.add_argument("--reward", type=int, default=50, help="genesis 奖励（单位 BTC，默认 50）")
    p.add_argument("--start-nonce", type=int, default=0, help="nonce 起始值")
    p.add_argument("--max-nonce", type=int, default=0xFFFFFFFF, help="nonce 最大值")
    return p.parse_args()


def main() -> int:
    args = parse_args()
    nbits = int(args.bits, 16)
    reward_sats = args.reward * 100_000_000

    nonce, genesis_hash_be, merkle_root_be = mine_genesis(
        timestamp=args.timestamp,
        pubkeyhex=args.pubkeyhex,
        ntime=args.time,
        nbits=nbits,
        version=args.version,
        reward_sats=reward_sats,
        start_nonce=args.start_nonce,
        max_nonce=args.max_nonce,
    )

    print("genesis_hash =", genesis_hash_be)
    print("merkle_root  =", merkle_root_be)
    print("nonce        =", nonce)
    print()
    print("可复制到 chainparams.cpp 的断言：")
    print(f'assert(consensus.hashGenesisBlock == uint256{{"{genesis_hash_be}"}});')
    print(f'assert(genesis.hashMerkleRoot == uint256{{"{merkle_root_be}"}});')
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
