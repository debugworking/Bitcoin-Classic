// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2022 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <pow.h>

#include <arith_uint256.h>
#include <cassert>
#include <chain.h>
#include <crypto/common.h>
#include <primitives/block.h>
#include <uint256.h>

#include <array>

static bool IsBTCCAsertEnabled(const Consensus::Params& params, int64_t height)
{
    return params.BTCCAsertHeight > 0 && height >= params.BTCCAsertHeight && params.BTCCAsertHalfLife > 0;
}

bool IsBTCCLegacyAsertEnabled(const Consensus::Params& params, int64_t height)
{
    return params.BTCCLegacyAsertHeight > 0 &&
           params.BTCCAsertHeight > params.BTCCLegacyAsertHeight &&
           height >= params.BTCCLegacyAsertHeight &&
           height < params.BTCCAsertHeight &&
           params.BTCCAsertHalfLife > 0;
}

static bool IsAnyBTCCAsertEnabled(const Consensus::Params& params, int64_t height)
{
    return IsBTCCLegacyAsertEnabled(params, height) || IsBTCCAsertEnabled(params, height);
}

static const CBlockIndex* GetBTCCAsertAnchor(const CBlockIndex* pindexLast, int anchor_height)
{
    if (anchor_height < 0 || pindexLast->nHeight < anchor_height) return nullptr;
    return pindexLast->GetAncestor(anchor_height);
}

static unsigned int ProductBits(const std::array<uint32_t, 9>& product)
{
    for (int pos = static_cast<int>(product.size()) - 1; pos >= 0; --pos) {
        if (product[pos] == 0) continue;
        for (int bit = 31; bit > 0; --bit) {
            if (product[pos] & (1U << bit)) return 32 * pos + bit + 1;
        }
        return 32 * pos + 1;
    }
    return 0;
}

static arith_uint256 ShiftProductToTarget(const std::array<uint32_t, 9>& product, int64_t shift)
{
    arith_uint256 result{0};
    for (size_t pos = 0; pos < product.size(); ++pos) {
        if (product[pos] == 0) continue;

        arith_uint256 part{product[pos]};
        const int64_t limb_shift = static_cast<int64_t>(pos) * 32 + shift;
        if (limb_shift >= 0) {
            if (limb_shift >= 256) continue;
            part <<= static_cast<unsigned int>(limb_shift);
        } else {
            const int64_t right_shift = -limb_shift;
            if (right_shift >= 32) continue;
            part >>= static_cast<unsigned int>(right_shift);
        }
        result |= part;
    }
    return result;
}

static arith_uint256 CalculateBTCCAsertTarget(const arith_uint256& anchor_target,
                                              uint64_t factor,
                                              int64_t shifts,
                                              const arith_uint256& pow_limit)
{
    // ASERT's factor is up to 17 bits, so the pre-shift product can need 273 bits.
    std::array<uint32_t, 9> product{};
    const uint256 anchor_blob = ArithToUint256(anchor_target);

    uint64_t carry = 0;
    for (size_t pos = 0; pos < 8; ++pos) {
        const uint64_t value =
            carry + uint64_t{ReadLE32(anchor_blob.begin() + pos * 4)} * factor;
        product[pos] = static_cast<uint32_t>(value & 0xffffffffU);
        carry = value >> 32;
    }
    product[8] = static_cast<uint32_t>(carry);

    const unsigned int product_bits = ProductBits(product);
    const int64_t final_shift = shifts - 16;
    const int64_t result_bits = static_cast<int64_t>(product_bits) + final_shift;
    if (product_bits == 0 || result_bits <= 0) return arith_uint256{1};
    if (result_bits > 256) return pow_limit;

    arith_uint256 next_target = ShiftProductToTarget(product, final_shift);
    if (next_target == 0) next_target = 1;
    if (next_target > pow_limit) next_target = pow_limit;
    return next_target;
}

static unsigned int CalculateBTCCAsertWorkRequired(const CBlockIndex* pindexLast, const Consensus::Params& params)
{
    assert(pindexLast != nullptr);

    const CBlockIndex* anchor = GetBTCCAsertAnchor(pindexLast, params.BTCCAsertHeight - 1);
    if (anchor == nullptr) return pindexLast->nBits;
    assert(anchor->pprev != nullptr);

    const int64_t height_delta = pindexLast->nHeight - anchor->nHeight;
    const int64_t time_delta = pindexLast->GetBlockTime() - anchor->pprev->GetBlockTime();
    const int64_t ideal_time_delta = (height_delta + 1) * params.nPowTargetSpacing;

    arith_uint256 anchor_target;
    anchor_target.SetCompact(anchor->nBits);

    // ASERT: target = anchor_target * 2^((time_delta - ideal_time_delta) / half_life)
    // BCH-style fixed-point integer approximation of 2^x with 16 fractional bits.
    static constexpr int64_t RADIX = 65536;
    const int64_t exponent = ((time_delta - ideal_time_delta) * RADIX) / params.BTCCAsertHalfLife;
    const int64_t shifts = exponent >> 16;
    const uint64_t frac = static_cast<uint64_t>(exponent - (shifts * RADIX));

    const uint64_t factor =
        RADIX + ((195766423245049ULL * frac + 971821376ULL * frac * frac +
                  5127ULL * frac * frac * frac + (1ULL << 47)) >> 48);
    const arith_uint256 pow_limit = UintToArith256(params.powLimit);
    const arith_uint256 next_target = CalculateBTCCAsertTarget(anchor_target, factor, shifts, pow_limit);
    return next_target.GetCompact();
}

static unsigned int CalculateBTCCLegacyAsertWorkRequired(const CBlockIndex* pindexLast,
                                                         const CBlockHeader* pblock,
                                                         const Consensus::Params& params)
{
    assert(pindexLast != nullptr);
    assert(pblock != nullptr);

    const CBlockIndex* anchor = GetBTCCAsertAnchor(pindexLast, params.BTCCLegacyAsertHeight - 1);
    if (anchor == nullptr) return pindexLast->nBits;

    const int next_height = pindexLast->nHeight + 1;
    const int64_t height_diff = next_height - anchor->nHeight;
    const int64_t time_diff = pblock->GetBlockTime() - anchor->GetBlockTime();
    const int64_t ideal_time_diff = height_diff * params.nPowTargetSpacing;

    arith_uint256 next_target;
    next_target.SetCompact(anchor->nBits);

    // Preserve the original BTCC ASERT semantics for already-mined blocks before
    // the BCH-style ASERT migration at BTCCAsertHeight.
    static constexpr int64_t RADIX = 65536;
    const int64_t exponent = ((time_diff - ideal_time_diff) * RADIX) / params.BTCCAsertHalfLife;
    const int64_t shifts = exponent >> 16;
    const uint64_t frac = static_cast<uint64_t>(exponent - (shifts * RADIX));

    const uint64_t factor =
        RADIX + ((195766423245049ULL * frac + 971821376ULL * frac * frac +
                  5127ULL * frac * frac * frac + (1ULL << 47)) >> 48);
    next_target *= static_cast<uint32_t>(factor);

    if (shifts <= -256) {
        next_target = 1;
    } else if (shifts < 0) {
        next_target >>= static_cast<unsigned int>(-shifts);
    } else if (shifts >= 256) {
        next_target = UintToArith256(params.powLimit);
    } else {
        next_target <<= static_cast<unsigned int>(shifts);
    }

    next_target >>= 16;

    if (next_target == 0) next_target = 1;

    const arith_uint256 pow_limit = UintToArith256(params.powLimit);
    if (next_target > pow_limit) next_target = pow_limit;

    return next_target.GetCompact();
}

unsigned int GetNextWorkRequired(const CBlockIndex* pindexLast, const CBlockHeader *pblock, const Consensus::Params& params)
{
    assert(pindexLast != nullptr);
    unsigned int nProofOfWorkLimit = UintToArith256(params.powLimit).GetCompact();

    const int next_height = pindexLast->nHeight + 1;
    if (IsBTCCLegacyAsertEnabled(params, next_height)) {
        return CalculateBTCCLegacyAsertWorkRequired(pindexLast, pblock, params);
    }

    if (IsBTCCAsertEnabled(params, next_height)) {
        return CalculateBTCCAsertWorkRequired(pindexLast, params);
    }

    // Only change once per difficulty adjustment interval
    if (next_height % params.DifficultyAdjustmentInterval() != 0)
    {
        if (params.fPowAllowMinDifficultyBlocks)
        {
            // Special difficulty rule for testnet:
            // If the new block's timestamp is more than 2* 10 minutes
            // then allow mining of a min-difficulty block.
            if (pblock->GetBlockTime() > pindexLast->GetBlockTime() + params.nPowTargetSpacing*2)
                return nProofOfWorkLimit;
            else
            {
                // Return the last non-special-min-difficulty-rules-block
                const CBlockIndex* pindex = pindexLast;
                while (pindex->pprev && pindex->nHeight % params.DifficultyAdjustmentInterval() != 0 && pindex->nBits == nProofOfWorkLimit)
                    pindex = pindex->pprev;
                return pindex->nBits;
            }
        }
        return pindexLast->nBits;
    }

    // Go back by what we want to be 14 days worth of blocks
    int nHeightFirst = pindexLast->nHeight - (params.DifficultyAdjustmentInterval()-1);
    assert(nHeightFirst >= 0);
    const CBlockIndex* pindexFirst = pindexLast->GetAncestor(nHeightFirst);
    assert(pindexFirst);

    return CalculateNextWorkRequired(pindexLast, pindexFirst->GetBlockTime(), params);
}

unsigned int CalculateNextWorkRequired(const CBlockIndex* pindexLast, int64_t nFirstBlockTime, const Consensus::Params& params)
{
    if (params.fPowNoRetargeting)
        return pindexLast->nBits;

    // Limit adjustment step
    int64_t nActualTimespan = pindexLast->GetBlockTime() - nFirstBlockTime;
    if (nActualTimespan < params.nPowTargetTimespan/4)
        nActualTimespan = params.nPowTargetTimespan/4;
    if (nActualTimespan > params.nPowTargetTimespan*4)
        nActualTimespan = params.nPowTargetTimespan*4;

    // Retarget
    const arith_uint256 bnPowLimit = UintToArith256(params.powLimit);
    arith_uint256 bnNew;

    // Special difficulty rule for Testnet4
    if (params.enforce_BIP94) {
        // Here we use the first block of the difficulty period. This way
        // the real difficulty is always preserved in the first block as
        // it is not allowed to use the min-difficulty exception.
        int nHeightFirst = pindexLast->nHeight - (params.DifficultyAdjustmentInterval()-1);
        const CBlockIndex* pindexFirst = pindexLast->GetAncestor(nHeightFirst);
        bnNew.SetCompact(pindexFirst->nBits);
    } else {
        bnNew.SetCompact(pindexLast->nBits);
    }

    bnNew *= nActualTimespan;
    bnNew /= params.nPowTargetTimespan;

    if (bnNew > bnPowLimit)
        bnNew = bnPowLimit;

    return bnNew.GetCompact();
}

// Check that on difficulty adjustments, the new difficulty does not increase
// or decrease beyond the permitted limits.
bool PermittedDifficultyTransition(const Consensus::Params& params, int64_t height, uint32_t old_nbits, uint32_t new_nbits)
{
    if (params.fPowAllowMinDifficultyBlocks) return true;

    if (IsAnyBTCCAsertEnabled(params, height)) return true;

    if (height % params.DifficultyAdjustmentInterval() == 0) {
        int64_t smallest_timespan = params.nPowTargetTimespan/4;
        int64_t largest_timespan = params.nPowTargetTimespan*4;

        const arith_uint256 pow_limit = UintToArith256(params.powLimit);
        arith_uint256 observed_new_target;
        observed_new_target.SetCompact(new_nbits);

        // Calculate the largest difficulty value possible:
        arith_uint256 largest_difficulty_target;
        largest_difficulty_target.SetCompact(old_nbits);
        largest_difficulty_target *= largest_timespan;
        largest_difficulty_target /= params.nPowTargetTimespan;

        if (largest_difficulty_target > pow_limit) {
            largest_difficulty_target = pow_limit;
        }

        // Round and then compare this new calculated value to what is
        // observed.
        arith_uint256 maximum_new_target;
        maximum_new_target.SetCompact(largest_difficulty_target.GetCompact());
        if (maximum_new_target < observed_new_target) return false;

        // Calculate the smallest difficulty value possible:
        arith_uint256 smallest_difficulty_target;
        smallest_difficulty_target.SetCompact(old_nbits);
        smallest_difficulty_target *= smallest_timespan;
        smallest_difficulty_target /= params.nPowTargetTimespan;

        if (smallest_difficulty_target > pow_limit) {
            smallest_difficulty_target = pow_limit;
        }

        // Round and then compare this new calculated value to what is
        // observed.
        arith_uint256 minimum_new_target;
        minimum_new_target.SetCompact(smallest_difficulty_target.GetCompact());
        if (minimum_new_target > observed_new_target) return false;
    } else if (old_nbits != new_nbits) {
        return false;
    }
    return true;
}

bool CheckProofOfWork(uint256 hash, unsigned int nBits, const Consensus::Params& params)
{
    bool fNegative;
    bool fOverflow;
    arith_uint256 bnTarget;

    bnTarget.SetCompact(nBits, &fNegative, &fOverflow);

    // Check range
    if (fNegative || bnTarget == 0 || fOverflow || bnTarget > UintToArith256(params.powLimit))
        return false;

    // Check proof of work matches claimed amount
    if (UintToArith256(hash) > bnTarget)
        return false;

    return true;
}
