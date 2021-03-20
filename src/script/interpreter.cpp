// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2014 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "interpreter.h"

#include "chain.h"
#include "crypto/ripemd160.h"
#include "crypto/sha1.h"
#include "crypto/sha256.h"
#include "pubkey.h"
#include "script/script.h"
#include "uint256.h"
#include "util.h"
#include "main.h"
#include <boost/algorithm/hex.hpp>
#include <boost/algorithm/string.hpp>

using namespace std;

namespace {

inline bool set_success(ScriptError* ret)
{
    if (ret)
        *ret = SCRIPT_ERR_OK;
    return true;
}

inline bool set_error(ScriptError* ret, const ScriptError serror)
{
    if (ret)
        *ret = serror;
    return false;
}

} // anon namespace

bool CastToBool(const valtype& vch)
{
    for (unsigned int i = 0; i < vch.size(); i++)
    {
        if (vch[i] != 0)
        {
            // Can be negative zero
            if (i == vch.size()-1 && vch[i] == 0x80)
                return false;
            return true;
        }
    }
    return false;
}

/**
 * Script is a stack machine (like Forth) that evaluates a predicate
 * returning a bool indicating valid or not.  There are no loops.
 */
#define stacktop(i)  (stack.at(stack.size()+(i)))
#define altstacktop(i)  (altstack.at(altstack.size()+(i)))
static inline void popstack(vector<valtype>& stack)
{
    if (stack.empty())
        throw runtime_error("popstack(): stack empty");
    stack.pop_back();
}

bool static IsCompressedOrUncompressedPubKey(const valtype &vchPubKey) {
    if (vchPubKey.size() < 33) {
        //  Non-canonical public key: too short
        return false;
    }
    if (vchPubKey[0] == 0x04) {
        if (vchPubKey.size() != 65) {
            //  Non-canonical public key: invalid length for uncompressed key
            return false;
        }
    } else if (vchPubKey[0] == 0x02 || vchPubKey[0] == 0x03) {
        if (vchPubKey.size() != 33) {
            //  Non-canonical public key: invalid length for compressed key
            return false;
        }
    } else {
          //  Non-canonical public key: neither compressed nor uncompressed
          return false;
    }
    return true;
}

/**
 * A canonical signature exists of: <30> <total len> <02> <len R> <R> <02> <len S> <S> <hashtype>
 * Where R and S are not negative (their first byte has its highest bit not set), and not
 * excessively padded (do not start with a 0 byte, unless an otherwise negative number follows,
 * in which case a single 0 byte is necessary and even required).
 * 
 * See https://bitcointalk.org/index.php?topic=8392.msg127623#msg127623
 *
 * This function is consensus-critical since BIP66.
 */
bool static IsValidSignatureEncoding(const std::vector<unsigned char> &sig) {
    // Format: 0x30 [total-length] 0x02 [R-length] [R] 0x02 [S-length] [S] [sighash]
    // * total-length: 1-byte length descriptor of everything that follows,
    //   excluding the sighash byte.
    // * R-length: 1-byte length descriptor of the R value that follows.
    // * R: arbitrary-length big-endian encoded R value. It must use the shortest
    //   possible encoding for a positive integers (which means no null bytes at
    //   the start, except a single one when the next byte has its highest bit set).
    // * S-length: 1-byte length descriptor of the S value that follows.
    // * S: arbitrary-length big-endian encoded S value. The same rules apply.
    // * sighash: 1-byte value indicating what data is hashed (not part of the DER
    //   signature)

    // Minimum and maximum size constraints.
    if (sig.size() < 9) return false;
    if (sig.size() > 73) return false;

    // A signature is of type 0x30 (compound).
    if (sig[0] != 0x30) return false;

    // Make sure the length covers the entire signature.
    if (sig[1] != sig.size() - 3) return false;

    // Extract the length of the R element.
    unsigned int lenR = sig[3];

    // Make sure the length of the S element is still inside the signature.
    if (5 + lenR >= sig.size()) return false;

    // Extract the length of the S element.
    unsigned int lenS = sig[5 + lenR];

    // Verify that the length of the signature matches the sum of the length
    // of the elements.
    if ((size_t)(lenR + lenS + 7) != sig.size()) return false;
 
    // Check whether the R element is an integer.
    if (sig[2] != 0x02) return false;

    // Zero-length integers are not allowed for R.
    if (lenR == 0) return false;

    // Negative numbers are not allowed for R.
    if (sig[4] & 0x80) return false;

    // Null bytes at the start of R are not allowed, unless R would
    // otherwise be interpreted as a negative number.
    if (lenR > 1 && (sig[4] == 0x00) && !(sig[5] & 0x80)) return false;

    // Check whether the S element is an integer.
    if (sig[lenR + 4] != 0x02) return false;

    // Zero-length integers are not allowed for S.
    if (lenS == 0) return false;

    // Negative numbers are not allowed for S.
    if (sig[lenR + 6] & 0x80) return false;

    // Null bytes at the start of S are not allowed, unless S would otherwise be
    // interpreted as a negative number.
    if (lenS > 1 && (sig[lenR + 6] == 0x00) && !(sig[lenR + 7] & 0x80)) return false;

    return true;
}

bool static IsLowDERSignature(const valtype &vchSig, ScriptError* serror) {
    if (!IsValidSignatureEncoding(vchSig)) {
        return set_error(serror, SCRIPT_ERR_SIG_DER);
    }
    // https://bitcoin.stackexchange.com/a/12556:
    //     Also note that inside transaction signatures, an extra hashtype byte
    //     follows the actual signature data.
    std::vector<unsigned char> vchSigCopy(vchSig.begin(), vchSig.begin() + vchSig.size() - 1);
    // If the S value is above the order of the curve divided by two, its
    // complement modulo the order could have been used instead, which is
    // one byte shorter when encoded correctly.
    return CPubKey::CheckLowS(vchSigCopy);
}

bool static IsDefinedHashtypeSignature(const valtype &vchSig) {
    if (vchSig.size() == 0) {
        return false;
    }
    unsigned char nHashType = vchSig[vchSig.size() - 1] & (~(SIGHASH_ANYONECANPAY));
    if (nHashType < SIGHASH_ALL || nHashType > SIGHASH_SINGLE)
        return false;

    return true;
}

bool static CheckSignatureEncoding(const valtype &vchSig, unsigned int flags, ScriptError* serror) {
    // Empty signature. Not strictly DER encoded, but allowed to provide a
    // compact way to provide an invalid signature for use with CHECK(MULTI)SIG
    if (vchSig.size() == 0) {
        return true;
    }
    if (!IsValidSignatureEncoding(vchSig)) {
        return set_error(serror, SCRIPT_ERR_SIG_DER);
    } else if ((flags & SCRIPT_VERIFY_LOW_S) != 0 && !IsLowDERSignature(vchSig, serror)) {
        // serror is set
        return false;
    } else if ((flags & SCRIPT_VERIFY_STRICTENC) != 0 && !IsDefinedHashtypeSignature(vchSig)) {
        return set_error(serror, SCRIPT_ERR_SIG_HASHTYPE);
    }
    return true;
}

bool static CheckPubKeyEncoding(const valtype &vchSig, unsigned int flags, ScriptError* serror) {
    if ((flags & SCRIPT_VERIFY_STRICTENC) != 0 && !IsCompressedOrUncompressedPubKey(vchSig)) {
        return set_error(serror, SCRIPT_ERR_PUBKEYTYPE);
    }
    return true;
}

bool CheckMinimalPush(const valtype& data, opcodetype opcode) {
    if (data.size() == 0) {
        // Could have used OP_0.
        return opcode == OP_0;
    } else if (data.size() == 1 && data[0] >= 1 && data[0] <= 16) {
        // Could have used OP_1 .. OP_16.
        return opcode == OP_1 + (data[0] - 1);
    } else if (data.size() == 1 && data[0] == 0x81) {
        // Could have used OP_1NEGATE.
        return opcode == OP_1NEGATE;
    } else if (data.size() <= 75) {
        // Could have used a direct push (opcode indicating number of bytes pushed + those bytes).
        return opcode == data.size();
    } else if (data.size() <= 255) {
        // Could have used OP_PUSHDATA.
        return opcode == OP_PUSHDATA1;
    } else if (data.size() <= 65535) {
        // Could have used OP_PUSHDATA2.
        return opcode == OP_PUSHDATA2;
    }
    return true;
}

bool EvalScript(vector<vector<unsigned char> >& stack, const CScript& script, unsigned int flags, const BaseSignatureChecker& checker, ScriptError* serror)
{
    static const CScriptNum bnZero(0);
    static const CScriptNum bnOne(1);
    static const CScriptNum bnFalse(0);
    static const CScriptNum bnTrue(1);
    static const valtype vchFalse(0);
    static const valtype vchZero(0);
    static const valtype vchTrue(1, 1);

    CScript::const_iterator pc = script.begin();
    CScript::const_iterator pend = script.end();
    opcodetype opcode;
    valtype vchPushValue;
    vector<bool> vfExec;
    vector<valtype> altstack;
    set_error(serror, SCRIPT_ERR_UNKNOWN_ERROR);
    if (script.size() > MAX_SCRIPT_SIZE)
        return set_error(serror, SCRIPT_ERR_SCRIPT_SIZE);
    int nOpCount = 0;
    bool fRequireMinimal = (flags & SCRIPT_VERIFY_MINIMALDATA) != 0;

    try
    {
        while (pc < pend)
        {
            bool fExec = !count(vfExec.begin(), vfExec.end(), false);

            //
            // Read instruction
            //
            if (!script.GetOp(pc, opcode, vchPushValue))
                return set_error(serror, SCRIPT_ERR_BAD_OPCODE);
            if (vchPushValue.size() > MAX_SCRIPT_ELEMENT_SIZE)
                return set_error(serror, SCRIPT_ERR_PUSH_SIZE);

            // Note how OP_RESERVED does not count towards the opcode limit.
            if (opcode > OP_16 && ++nOpCount > 201)
                return set_error(serror, SCRIPT_ERR_OP_COUNT);

            if (opcode == OP_CAT ||
                opcode == OP_SUBSTR ||
                opcode == OP_LEFT ||
                opcode == OP_RIGHT ||
                opcode == OP_INVERT ||
                opcode == OP_AND ||
                opcode == OP_OR ||
                opcode == OP_XOR ||
                opcode == OP_2MUL ||
                opcode == OP_2DIV ||
                opcode == OP_MUL ||
                opcode == OP_DIV ||
                opcode == OP_MOD ||
                opcode == OP_LSHIFT ||
                opcode == OP_RSHIFT ||
                opcode == OP_CODESEPARATOR)
                return set_error(serror, SCRIPT_ERR_DISABLED_OPCODE); // Disabled opcodes.

            if (fExec && 0 <= opcode && opcode <= OP_PUSHDATA4) {
                if (fRequireMinimal && !CheckMinimalPush(vchPushValue, opcode)) {
                    return set_error(serror, SCRIPT_ERR_MINIMALDATA);
                }
                stack.push_back(vchPushValue);
            } else if (fExec || (OP_IF <= opcode && opcode <= OP_ENDIF))
            switch (opcode)
            {
                //
                // Push value
                //
                case OP_1NEGATE:
                case OP_1:
                case OP_2:
                case OP_3:
                case OP_4:
                case OP_5:
                case OP_6:
                case OP_7:
                case OP_8:
                case OP_9:
                case OP_10:
                case OP_11:
                case OP_12:
                case OP_13:
                case OP_14:
                case OP_15:
                case OP_16:
                {
                    // ( -- value)
                    CScriptNum bn((int)opcode - (int)(OP_1 - 1));
                    stack.push_back(bn.getvch());
                    // The result of these opcodes should always be the minimal way to push the data
                    // they push, so no need for a CheckMinimalPush here.
                }
                break;


                //
                // Control
                //
                case OP_NOP:
                    break;

                case OP_CHECKLOCKTIMEVERIFY:
                {
                    if (!(flags & SCRIPT_VERIFY_CHECKLOCKTIMEVERIFY)) {
                        // not enabled; treat as a NOP2
                        if (flags & SCRIPT_VERIFY_DISCOURAGE_UPGRADABLE_NOPS) {
                            return set_error(serror, SCRIPT_ERR_DISCOURAGE_UPGRADABLE_NOPS);
                        }
                        break;
                    }

                    if (stack.size() < 1)
                        return set_error(serror, SCRIPT_ERR_INVALID_STACK_OPERATION);

                    // Note that elsewhere numeric opcodes are limited to
                    // operands in the range -2**31+1 to 2**31-1, however it is
                    // legal for opcodes to produce results exceeding that
                    // range. This limitation is implemented by CScriptNum's
                    // default 4-byte limit.
                    //
                    // If we kept to that limit we'd have a year 2038 problem,
                    // even though the nLockTime field in transactions
                    // themselves is uint32 which only becomes meaningless
                    // after the year 2106.
                    //
                    // Thus as a special case we tell CScriptNum to accept up
                    // to 5-byte bignums, which are good until 2**39-1, well
                    // beyond the 2**32-1 limit of the nLockTime field itself.
                    const CScriptNum nLockTime(stacktop(-1), fRequireMinimal, 5);

                    // In the rare event that the argument may be < 0 due to
                    // some arithmetic being done first, you can always use
                    // 0 MAX CHECKLOCKTIMEVERIFY.
                    if (nLockTime < 0)
                        return set_error(serror, SCRIPT_ERR_NEGATIVE_LOCKTIME);

                    // Actually compare the specified lock time with the transaction.
                    if (!checker.CheckLockTime(nLockTime))
                        return set_error(serror, SCRIPT_ERR_UNSATISFIED_LOCKTIME);

                    break;
                }

                case OP_CHECKBLOCKATHEIGHT:
                {
                    if (!(flags & SCRIPT_VERIFY_CHECKBLOCKATHEIGHT)) {
                        // At least check that there are 2 parameters
                        if (stack.size() < 2) {
                            LogPrintf("%s: %s: OP_CHECKBLOCKATHEIGHT verification failed. Wrong parameters amount.\n", __FILE__, __func__);
                            return set_error(serror, SCRIPT_ERR_INVALID_STACK_OPERATION);
                        }
                        // Clear stack
                        popstack(stack);
                        popstack(stack);
                        break;
                    }

                    if (stack.size() < 2) {
                        LogPrintf("%s: %s: OP_CHECKBLOCKATHEIGHT verification failed. Wrong parameters amount.\n", __FILE__, __func__);
                        return set_error(serror, SCRIPT_ERR_INVALID_STACK_OPERATION);
                    }

                    valtype vchBlockHash(stacktop(-2));
                    valtype vchBlockIndex(stacktop(-1));

                    if ((vchBlockIndex.size() > sizeof(int)) || (vchBlockHash.size() > 32))
                    {
                        LogPrintf("%s: %s():%d - OP_CHECKBLOCKATHEIGHT verification failed. Bad params.\n", __FILE__, __func__, __LINE__);
                        return set_error(serror, SCRIPT_ERR_CHECKBLOCKATHEIGHT);
                    }

                    // nHeight is a 32-bit signed integer field.
                    const int32_t nHeight = CScriptNum(vchBlockIndex, true, 4).getint();

                    // Compare the specified block hash with the input.
                    if (nHeight < 0 || !checker.CheckBlockHash(nHeight, vchBlockHash)) {
                        // Not final rather than a hard reject to avoid caching across different blockchains
                        // Also because it will *eventually* become final when the height gets old enough
                        LogPrintf("%s: %s():%d - OP_CHECKBLOCKATHEIGHT verification failed. Referenced height: %d\n",
                            __FILE__, __func__, __LINE__, nHeight);
                        return set_error(serror, SCRIPT_ERR_NOT_FINAL);
                    }

                    // Clear stack
                    popstack(stack);
                    popstack(stack);

                    break;
                }

                case OP_NOP1: case OP_NOP3: case OP_NOP4:
                case OP_NOP6: case OP_NOP7: case OP_NOP8: case OP_NOP9: case OP_NOP10:
                {
                    if (flags & SCRIPT_VERIFY_DISCOURAGE_UPGRADABLE_NOPS)
                        return set_error(serror, SCRIPT_ERR_DISCOURAGE_UPGRADABLE_NOPS);
                }
                break;

                case OP_IF:
                case OP_NOTIF:
                {
                    // <expression> if [statements] [else [statements]] endif
                    bool fValue = false;
                    if (fExec)
                    {
                        if (stack.size() < 1)
                            return set_error(serror, SCRIPT_ERR_UNBALANCED_CONDITIONAL);
                        valtype& vch = stacktop(-1);
                        fValue = CastToBool(vch);
                        if (opcode == OP_NOTIF)
                            fValue = !fValue;
                        popstack(stack);
                    }
                    vfExec.push_back(fValue);
                }
                break;

                case OP_ELSE:
                {
                    if (vfExec.empty())
                        return set_error(serror, SCRIPT_ERR_UNBALANCED_CONDITIONAL);
                    vfExec.back() = !vfExec.back();
                }
                break;

                case OP_ENDIF:
                {
                    if (vfExec.empty())
                        return set_error(serror, SCRIPT_ERR_UNBALANCED_CONDITIONAL);
                    vfExec.pop_back();
                }
                break;

                case OP_VERIFY:
                {
                    // (true -- ) or
                    // (false -- false) and return
                    if (stack.size() < 1)
                        return set_error(serror, SCRIPT_ERR_INVALID_STACK_OPERATION);
                    bool fValue = CastToBool(stacktop(-1));
                    if (fValue)
                        popstack(stack);
                    else
                        return set_error(serror, SCRIPT_ERR_VERIFY);
                }
                break;

                case OP_RETURN:
                {
                    return set_error(serror, SCRIPT_ERR_OP_RETURN);
                }
                break;


                //
                // Stack ops
                //
                case OP_TOALTSTACK:
                {
                    if (stack.size() < 1)
                        return set_error(serror, SCRIPT_ERR_INVALID_STACK_OPERATION);
                    altstack.push_back(stacktop(-1));
                    popstack(stack);
                }
                break;

                case OP_FROMALTSTACK:
                {
                    if (altstack.size() < 1)
                        return set_error(serror, SCRIPT_ERR_INVALID_ALTSTACK_OPERATION);
                    stack.push_back(altstacktop(-1));
                    popstack(altstack);
                }
                break;

                case OP_2DROP:
                {
                    // (x1 x2 -- )
                    if (stack.size() < 2)
                        return set_error(serror, SCRIPT_ERR_INVALID_STACK_OPERATION);
                    popstack(stack);
                    popstack(stack);
                }
                break;

                case OP_2DUP:
                {
                    // (x1 x2 -- x1 x2 x1 x2)
                    if (stack.size() < 2)
                        return set_error(serror, SCRIPT_ERR_INVALID_STACK_OPERATION);
                    valtype vch1 = stacktop(-2);
                    valtype vch2 = stacktop(-1);
                    stack.push_back(vch1);
                    stack.push_back(vch2);
                }
                break;

                case OP_3DUP:
                {
                    // (x1 x2 x3 -- x1 x2 x3 x1 x2 x3)
                    if (stack.size() < 3)
                        return set_error(serror, SCRIPT_ERR_INVALID_STACK_OPERATION);
                    valtype vch1 = stacktop(-3);
                    valtype vch2 = stacktop(-2);
                    valtype vch3 = stacktop(-1);
                    stack.push_back(vch1);
                    stack.push_back(vch2);
                    stack.push_back(vch3);
                }
                break;

                case OP_2OVER:
                {
                    // (x1 x2 x3 x4 -- x1 x2 x3 x4 x1 x2)
                    if (stack.size() < 4)
                        return set_error(serror, SCRIPT_ERR_INVALID_STACK_OPERATION);
                    valtype vch1 = stacktop(-4);
                    valtype vch2 = stacktop(-3);
                    stack.push_back(vch1);
                    stack.push_back(vch2);
                }
                break;

                case OP_2ROT:
                {
                    // (x1 x2 x3 x4 x5 x6 -- x3 x4 x5 x6 x1 x2)
                    if (stack.size() < 6)
                        return set_error(serror, SCRIPT_ERR_INVALID_STACK_OPERATION);
                    valtype vch1 = stacktop(-6);
                    valtype vch2 = stacktop(-5);
                    stack.erase(stack.end()-6, stack.end()-4);
                    stack.push_back(vch1);
                    stack.push_back(vch2);
                }
                break;

                case OP_2SWAP:
                {
                    // (x1 x2 x3 x4 -- x3 x4 x1 x2)
                    if (stack.size() < 4)
                        return set_error(serror, SCRIPT_ERR_INVALID_STACK_OPERATION);
                    swap(stacktop(-4), stacktop(-2));
                    swap(stacktop(-3), stacktop(-1));
                }
                break;

                case OP_IFDUP:
                {
                    // (x - 0 | x x)
                    if (stack.size() < 1)
                        return set_error(serror, SCRIPT_ERR_INVALID_STACK_OPERATION);
                    valtype vch = stacktop(-1);
                    if (CastToBool(vch))
                        stack.push_back(vch);
                }
                break;

                case OP_DEPTH:
                {
                    // -- stacksize
                    CScriptNum bn(stack.size());
                    stack.push_back(bn.getvch());
                }
                break;

                case OP_DROP:
                {
                    // (x -- )
                    if (stack.size() < 1)
                        return set_error(serror, SCRIPT_ERR_INVALID_STACK_OPERATION);
                    popstack(stack);
                }
                break;

                case OP_DUP:
                {
                    // (x -- x x)
                    if (stack.size() < 1)
                        return set_error(serror, SCRIPT_ERR_INVALID_STACK_OPERATION);
                    valtype vch = stacktop(-1);
                    stack.push_back(vch);
                }
                break;

                case OP_NIP:
                {
                    // (x1 x2 -- x2)
                    if (stack.size() < 2)
                        return set_error(serror, SCRIPT_ERR_INVALID_STACK_OPERATION);
                    stack.erase(stack.end() - 2);
                }
                break;

                case OP_OVER:
                {
                    // (x1 x2 -- x1 x2 x1)
                    if (stack.size() < 2)
                        return set_error(serror, SCRIPT_ERR_INVALID_STACK_OPERATION);
                    valtype vch = stacktop(-2);
                    stack.push_back(vch);
                }
                break;

                case OP_PICK:
                case OP_ROLL:
                {
                    // (xn ... x2 x1 x0 n - xn ... x2 x1 x0 xn)
                    // (xn ... x2 x1 x0 n - ... x2 x1 x0 xn)
                    if (stack.size() < 2)
                        return set_error(serror, SCRIPT_ERR_INVALID_STACK_OPERATION);
                    int n = CScriptNum(stacktop(-1), fRequireMinimal).getint();
                    popstack(stack);
                    if (n < 0 || n >= (int)stack.size())
                        return set_error(serror, SCRIPT_ERR_INVALID_STACK_OPERATION);
                    valtype vch = stacktop(-n-1);
                    if (opcode == OP_ROLL)
                        stack.erase(stack.end()-n-1);
                    stack.push_back(vch);
                }
                break;

                case OP_ROT:
                {
                    // (x1 x2 x3 -- x2 x3 x1)
                    //  x2 x1 x3  after first swap
                    //  x2 x3 x1  after second swap
                    if (stack.size() < 3)
                        return set_error(serror, SCRIPT_ERR_INVALID_STACK_OPERATION);
                    swap(stacktop(-3), stacktop(-2));
                    swap(stacktop(-2), stacktop(-1));
                }
                break;

                case OP_SWAP:
                {
                    // (x1 x2 -- x2 x1)
                    if (stack.size() < 2)
                        return set_error(serror, SCRIPT_ERR_INVALID_STACK_OPERATION);
                    swap(stacktop(-2), stacktop(-1));
                }
                break;

                case OP_TUCK:
                {
                    // (x1 x2 -- x2 x1 x2)
                    if (stack.size() < 2)
                        return set_error(serror, SCRIPT_ERR_INVALID_STACK_OPERATION);
                    valtype vch = stacktop(-1);
                    stack.insert(stack.end()-2, vch);
                }
                break;


                case OP_SIZE:
                {
                    // (in -- in size)
                    if (stack.size() < 1)
                        return set_error(serror, SCRIPT_ERR_INVALID_STACK_OPERATION);
                    CScriptNum bn(stacktop(-1).size());
                    stack.push_back(bn.getvch());
                }
                break;


                //
                // Bitwise logic
                //
                case OP_EQUAL:
                case OP_EQUALVERIFY:
                //case OP_NOTEQUAL: // use OP_NUMNOTEQUAL
                {
                    // (x1 x2 - bool)
                    if (stack.size() < 2)
                        return set_error(serror, SCRIPT_ERR_INVALID_STACK_OPERATION);
                    valtype& vch1 = stacktop(-2);
                    valtype& vch2 = stacktop(-1);
                    bool fEqual = (vch1 == vch2);
                    // OP_NOTEQUAL is disabled because it would be too easy to say
                    // something like n != 1 and have some wiseguy pass in 1 with extra
                    // zero bytes after it (numerically, 0x01 == 0x0001 == 0x000001)
                    //if (opcode == OP_NOTEQUAL)
                    //    fEqual = !fEqual;
                    popstack(stack);
                    popstack(stack);
                    stack.push_back(fEqual ? vchTrue : vchFalse);
                    if (opcode == OP_EQUALVERIFY)
                    {
                        if (fEqual)
                            popstack(stack);
                        else
                            return set_error(serror, SCRIPT_ERR_EQUALVERIFY);
                    }
                }
                break;


                //
                // Numeric
                //
                case OP_1ADD:
                case OP_1SUB:
                case OP_NEGATE:
                case OP_ABS:
                case OP_NOT:
                case OP_0NOTEQUAL:
                {
                    // (in -- out)
                    if (stack.size() < 1)
                        return set_error(serror, SCRIPT_ERR_INVALID_STACK_OPERATION);
                    CScriptNum bn(stacktop(-1), fRequireMinimal);
                    switch (opcode)
                    {
                    case OP_1ADD:       bn += bnOne; break;
                    case OP_1SUB:       bn -= bnOne; break;
                    case OP_NEGATE:     bn = -bn; break;
                    case OP_ABS:        if (bn < bnZero) bn = -bn; break;
                    case OP_NOT:        bn = (bn == bnZero); break;
                    case OP_0NOTEQUAL:  bn = (bn != bnZero); break;
                    default:            assert(!"invalid opcode"); break;
                    }
                    popstack(stack);
                    stack.push_back(bn.getvch());
                }
                break;

                case OP_ADD:
                case OP_SUB:
                case OP_BOOLAND:
                case OP_BOOLOR:
                case OP_NUMEQUAL:
                case OP_NUMEQUALVERIFY:
                case OP_NUMNOTEQUAL:
                case OP_LESSTHAN:
                case OP_GREATERTHAN:
                case OP_LESSTHANOREQUAL:
                case OP_GREATERTHANOREQUAL:
                case OP_MIN:
                case OP_MAX:
                {
                    // (x1 x2 -- out)
                    if (stack.size() < 2)
                        return set_error(serror, SCRIPT_ERR_INVALID_STACK_OPERATION);
                    CScriptNum bn1(stacktop(-2), fRequireMinimal);
                    CScriptNum bn2(stacktop(-1), fRequireMinimal);
                    CScriptNum bn(0);
                    switch (opcode)
                    {
                    case OP_ADD:
                        bn = bn1 + bn2;
                        break;

                    case OP_SUB:
                        bn = bn1 - bn2;
                        break;

                    case OP_BOOLAND:             bn = (bn1 != bnZero && bn2 != bnZero); break;
                    case OP_BOOLOR:              bn = (bn1 != bnZero || bn2 != bnZero); break;
                    case OP_NUMEQUAL:            bn = (bn1 == bn2); break;
                    case OP_NUMEQUALVERIFY:      bn = (bn1 == bn2); break;
                    case OP_NUMNOTEQUAL:         bn = (bn1 != bn2); break;
                    case OP_LESSTHAN:            bn = (bn1 < bn2); break;
                    case OP_GREATERTHAN:         bn = (bn1 > bn2); break;
                    case OP_LESSTHANOREQUAL:     bn = (bn1 <= bn2); break;
                    case OP_GREATERTHANOREQUAL:  bn = (bn1 >= bn2); break;
                    case OP_MIN:                 bn = (bn1 < bn2 ? bn1 : bn2); break;
                    case OP_MAX:                 bn = (bn1 > bn2 ? bn1 : bn2); break;
                    default:                     assert(!"invalid opcode"); break;
                    }
                    popstack(stack);
                    popstack(stack);
                    stack.push_back(bn.getvch());

                    if (opcode == OP_NUMEQUALVERIFY)
                    {
                        if (CastToBool(stacktop(-1)))
                            popstack(stack);
                        else
                            return set_error(serror, SCRIPT_ERR_NUMEQUALVERIFY);
                    }
                }
                break;

                case OP_WITHIN:
                {
                    // (x min max -- out)
                    if (stack.size() < 3)
                        return set_error(serror, SCRIPT_ERR_INVALID_STACK_OPERATION);
                    CScriptNum bn1(stacktop(-3), fRequireMinimal);
                    CScriptNum bn2(stacktop(-2), fRequireMinimal);
                    CScriptNum bn3(stacktop(-1), fRequireMinimal);
                    bool fValue = (bn2 <= bn1 && bn1 < bn3);
                    popstack(stack);
                    popstack(stack);
                    popstack(stack);
                    stack.push_back(fValue ? vchTrue : vchFalse);
                }
                break;


                //
                // Crypto
                //
                case OP_RIPEMD160:
                case OP_SHA1:
                case OP_SHA256:
                case OP_HASH160:
                case OP_HASH256:
                {
                    // (in -- hash)
                    if (stack.size() < 1)
                        return set_error(serror, SCRIPT_ERR_INVALID_STACK_OPERATION);
                    valtype& vch = stacktop(-1);
                    valtype vchHash((opcode == OP_RIPEMD160 || opcode == OP_SHA1 || opcode == OP_HASH160) ? 20 : 32);
                    if (opcode == OP_RIPEMD160)
                        CRIPEMD160().Write(begin_ptr(vch), vch.size()).Finalize(begin_ptr(vchHash));
                    else if (opcode == OP_SHA1)
                        CSHA1().Write(begin_ptr(vch), vch.size()).Finalize(begin_ptr(vchHash));
                    else if (opcode == OP_SHA256)
                        CSHA256().Write(begin_ptr(vch), vch.size()).Finalize(begin_ptr(vchHash));
                    else if (opcode == OP_HASH160)
                        CHash160().Write(begin_ptr(vch), vch.size()).Finalize(begin_ptr(vchHash));
                    else if (opcode == OP_HASH256)
                        CHash256().Write(begin_ptr(vch), vch.size()).Finalize(begin_ptr(vchHash));
                    popstack(stack);
                    stack.push_back(vchHash);
                }
                break;

                case OP_CHECKSIG:
                case OP_CHECKSIGVERIFY:
                {
                    // (sig pubkey -- bool)
                    if (stack.size() < 2)
                        return set_error(serror, SCRIPT_ERR_INVALID_STACK_OPERATION);

                    valtype& vchSig    = stacktop(-2);
                    valtype& vchPubKey = stacktop(-1);

                    if (!CheckSignatureEncoding(vchSig, flags, serror) || !CheckPubKeyEncoding(vchPubKey, flags, serror)) {
                        //serror is set
                        return false;
                    }
                    bool fSuccess = checker.CheckSig(vchSig, vchPubKey, script);

                    popstack(stack);
                    popstack(stack);
                    stack.push_back(fSuccess ? vchTrue : vchFalse);
                    if (opcode == OP_CHECKSIGVERIFY)
                    {
                        if (fSuccess)
                            popstack(stack);
                        else
                            return set_error(serror, SCRIPT_ERR_CHECKSIGVERIFY);
                    }
                }
                break;

                case OP_CHECKMULTISIG:
                case OP_CHECKMULTISIGVERIFY:
                {
                    // ([sig ...] num_of_signatures [pubkey ...] num_of_pubkeys -- bool)

                    int i = 1;
                    if ((int)stack.size() < i)
                        return set_error(serror, SCRIPT_ERR_INVALID_STACK_OPERATION);

                    int nKeysCount = CScriptNum(stacktop(-i), fRequireMinimal).getint();
                    if (nKeysCount < 0 || nKeysCount > 20)
                        return set_error(serror, SCRIPT_ERR_PUBKEY_COUNT);
                    nOpCount += nKeysCount;
                    if (nOpCount > 201)
                        return set_error(serror, SCRIPT_ERR_OP_COUNT);
                    int ikey = ++i;
                    i += nKeysCount;
                    if ((int)stack.size() < i)
                        return set_error(serror, SCRIPT_ERR_INVALID_STACK_OPERATION);

                    int nSigsCount = CScriptNum(stacktop(-i), fRequireMinimal).getint();
                    if (nSigsCount < 0 || nSigsCount > nKeysCount)
                        return set_error(serror, SCRIPT_ERR_SIG_COUNT);
                    int isig = ++i;
                    i += nSigsCount;
                    if ((int)stack.size() < i)
                        return set_error(serror, SCRIPT_ERR_INVALID_STACK_OPERATION);

                    bool fSuccess = true;
                    while (fSuccess && nSigsCount > 0)
                    {
                        valtype& vchSig    = stacktop(-isig);
                        valtype& vchPubKey = stacktop(-ikey);

                        // Note how this makes the exact order of pubkey/signature evaluation
                        // distinguishable by CHECKMULTISIG NOT if the STRICTENC flag is set.
                        // See the script_(in)valid tests for details.
                        if (!CheckSignatureEncoding(vchSig, flags, serror) || !CheckPubKeyEncoding(vchPubKey, flags, serror)) {
                            // serror is set
                            return false;
                        }

                        // Check signature
                        bool fOk = checker.CheckSig(vchSig, vchPubKey, script);

                        if (fOk) {
                            isig++;
                            nSigsCount--;
                        }
                        ikey++;
                        nKeysCount--;

                        // If there are more signatures left than keys left,
                        // then too many signatures have failed. Exit early,
                        // without checking any further signatures.
                        if (nSigsCount > nKeysCount)
                            fSuccess = false;
                    }

                    // Clean up stack of actual arguments
                    while (i-- > 1)
                        popstack(stack);

                    // A bug causes CHECKMULTISIG to consume one extra argument
                    // whose contents were not checked in any way.
                    //
                    // Unfortunately this is a potential source of mutability,
                    // so optionally verify it is exactly equal to zero prior
                    // to removing it from the stack.
                    if (stack.size() < 1)
                        return set_error(serror, SCRIPT_ERR_INVALID_STACK_OPERATION);
                    if ((flags & SCRIPT_VERIFY_NULLDUMMY) && stacktop(-1).size())
                        return set_error(serror, SCRIPT_ERR_SIG_NULLDUMMY);
                    popstack(stack);

                    stack.push_back(fSuccess ? vchTrue : vchFalse);

                    if (opcode == OP_CHECKMULTISIGVERIFY)
                    {
                        if (fSuccess)
                            popstack(stack);
                        else
                            return set_error(serror, SCRIPT_ERR_CHECKMULTISIGVERIFY);
                    }
                }
                break;

                default:
                    return set_error(serror, SCRIPT_ERR_BAD_OPCODE);
            }

            // Size limits
            if (stack.size() + altstack.size() > 1000)
                return set_error(serror, SCRIPT_ERR_STACK_SIZE);
        }
    }
    catch (...)
    {
        return set_error(serror, SCRIPT_ERR_UNKNOWN_ERROR);
    }

    if (!vfExec.empty())
        return set_error(serror, SCRIPT_ERR_UNBALANCED_CONDITIONAL);

    return set_success(serror);
}

namespace {

/**
 * Wrapper that serializes like CTransaction, but with the modifications
 *  required for the signature hash done in-place
 */
class CTransactionSignatureSerializer {
private:
    const CTransactionBase &txBaseTo;  //! reference to the spending transaction (the one being serialized)
    const CScript &scriptCode; //! output script being consumed
    const unsigned int nIn;    //! input index (both regular and CSW) of txBaseTo being signed
    const bool fAnyoneCanPay;  //! whether the hashtype has the SIGHASH_ANYONECANPAY flag set
    const bool fHashSingle;    //! whether the hashtype is SIGHASH_SINGLE
    const bool fHashNone;      //! whether the hashtype is SIGHASH_NONE

public:
    CTransactionSignatureSerializer(const CTransactionBase &txToIn, const CScript &scriptCodeIn, unsigned int nInIn, int nHashTypeIn) :
        txBaseTo(txToIn), scriptCode(scriptCodeIn), nIn(nInIn),
        fAnyoneCanPay(!!(nHashTypeIn & SIGHASH_ANYONECANPAY)),
        fHashSingle((nHashTypeIn & 0x1f) == SIGHASH_SINGLE),
        fHashNone((nHashTypeIn & 0x1f) == SIGHASH_NONE) {}

    /** Serialize the passed scriptCode */
    template<typename S>
    void SerializeScriptCode(S &s, int nType, int nVersion) const {
        auto size = scriptCode.size();
        ::WriteCompactSize(s, size);
        s.write((char*)&scriptCode.begin()[0], size);
    }

    /** Serialize an input of txTo */
    template<typename S>
    void SerializeInput(S &s, unsigned int nInput, int nType, int nVersion) const {
        // In case of SIGHASH_ANYONECANPAY, only the input being signed is serialized
        if (fAnyoneCanPay)
            nInput = nIn;
        // Serialize the prevout
        ::Serialize(s, txBaseTo.GetVin()[nInput].prevout, nType, nVersion);
        // Serialize the script
        assert(nInput != NOT_AN_INPUT);
        if (nInput != nIn)
            // Blank out other inputs' signatures
            ::Serialize(s, CScript(), nType, nVersion);
        else
            SerializeScriptCode(s, nType, nVersion);
        // Serialize the nSequence
        if (nInput != nIn && (fHashSingle || fHashNone))
            // let the others update at will
            ::Serialize(s, (int)0, nType, nVersion);
        else
            ::Serialize(s, txBaseTo.GetVin()[nInput].nSequence, nType, nVersion);
    }
    
    /** Serialize an output of txTo */
    template<typename S>
    void SerializeOutput(S &s, unsigned int nOutput, int nType, int nVersion) const {
        if (fHashSingle && nOutput != nIn)
            // Do not lock-in the txout payee at other indices as txin
            ::Serialize(s, CTxOut(), nType, nVersion);
        else
            ::Serialize(s, txBaseTo.GetVout()[nOutput], nType, nVersion);
    }
 
    /** Serialize a CSW input of txTo */
    template<typename S>
    void SerializeCswInput(S &s, unsigned int nCswInput, const CTransaction& txTo, int nType, int nVersion) const {
        // In case of SIGHASH_ANYONECANPAY, only the CSW input being signed is serialized
        if (fAnyoneCanPay)
            nCswInput = nIn - txTo.GetVin().size();
        // Serialize all CSW input fields except redeemScript
        ::Serialize(s, txTo.GetVcswCcIn()[nCswInput].nValue, nType, nVersion);
        ::Serialize(s, txTo.GetVcswCcIn()[nCswInput].scId, nType, nVersion);
        ::Serialize(s, txTo.GetVcswCcIn()[nCswInput].nullifier, nType, nVersion);
        ::Serialize(s, txTo.GetVcswCcIn()[nCswInput].pubKeyHash, nType, nVersion);
        ::Serialize(s, txTo.GetVcswCcIn()[nCswInput].scProof, nType, nVersion);

        // Serialize the script
        unsigned int nTotalInIdx = nCswInput + txTo.GetVin().size();
        assert(nTotalInIdx != NOT_AN_INPUT);
        if (nTotalInIdx != nIn)
            // Blank out other inputs' signatures
            ::Serialize(s, CScript(), nType, nVersion);
        else
            SerializeScriptCode(s, nType, nVersion);
    }

    /** Serialize txTo */
    template<typename S>
    void Serialize(S &s, int nType, int nVersion) const {

        // Serialize nVersion for both tx and cert
        ::Serialize(s, txBaseTo.nVersion, nType, nVersion);

        if (!txBaseTo.IsCertificate() ) {
            const CTransaction& txTo = dynamic_cast<const CTransaction&>(txBaseTo);

            // Serialize vin
            // In case of SIGHASH_ANYONECANPAY:
            // * if Tx has Sc support version and nIn belongs to the CSW inputs - skip inputs serialization
            // * otherwise only the input being signed is serialized
            // Otherwise - serialize all inputs
            unsigned int nInputs = fAnyoneCanPay ?
                        (txTo.IsScVersion() && nIn >= txTo.GetVin().size() ? 0 : 1) :
                        txTo.GetVin().size();
            ::WriteCompactSize(s, nInputs);
            for (unsigned int nInput = 0; nInput < nInputs; nInput++)
                SerializeInput(s, nInput, nType, nVersion);
            // Serialize vout
            unsigned int nOutputs = fHashNone ? 0 : (fHashSingle ? nIn+1 : txTo.GetVout().size());
            ::WriteCompactSize(s, nOutputs);
            for (unsigned int nOutput = 0; nOutput < nOutputs; nOutput++)
                 SerializeOutput(s, nOutput, nType, nVersion);
 
            if (txTo.IsScVersion() )
            {
                // Serialize CSW inputs
                // In case of SIGHASH_ANYONECANPAY:
                // * if Tx has Sc support version and nIn belongs to the CSW inputs - only the CSW input being signed is serialized
                // * otherwise skip CSW inputs
                // Otherwise - serialize all CSW inputs
                unsigned int nCswInputs = fAnyoneCanPay ?
                            (nIn < txTo.GetVin().size() ? 0 : 1) :
                            txTo.GetVcswCcIn().size();
                ::WriteCompactSize(s, nCswInputs);
                for (unsigned int nCswInput = 0; nCswInput < nCswInputs; nCswInput++)
                    SerializeCswInput(s, nCswInput, txTo, nType, nVersion);

                // Serialize vccouts
                unsigned int nCcOutputs = 0;
 
                nCcOutputs = fHashNone ? 0 : (txTo.GetVscCcOut().size());
                ::WriteCompactSize(s, nCcOutputs);
                for (unsigned int nCcOutput = 0; nCcOutput < nCcOutputs; nCcOutput++)
                    ::Serialize(s, txTo.GetVscCcOut()[nCcOutput], nType, nVersion);
 
                nCcOutputs = fHashNone ? 0 : (txTo.GetVftCcOut().size());
                ::WriteCompactSize(s, nCcOutputs);
                for (unsigned int nCcOutput = 0; nCcOutput < nCcOutputs; nCcOutput++)
                    ::Serialize(s, txTo.GetVftCcOut()[nCcOutput], nType, nVersion);

                nCcOutputs = fHashNone ? 0 : (txTo.GetVBwtRequestOut().size());
                ::WriteCompactSize(s, nCcOutputs);
                for (unsigned int nCcOutput = 0; nCcOutput < nCcOutputs; nCcOutput++)
                    ::Serialize(s, txTo.GetVBwtRequestOut()[nCcOutput], nType, nVersion);
            }
 
            // Serialize nLockTime
            ::Serialize(s, txTo.GetLockTime(), nType, nVersion);
 
            // Serialize vjoinsplit
            if (txTo.nVersion >= PHGR_TX_VERSION || txTo.nVersion == GROTH_TX_VERSION) {
                //
                // SIGHASH_* functions will hash portions of
                // the transaction for use in signatures. This
                // keeps the JoinSplit cryptographically bound
                // to the transaction.
                //
                auto os = WithTxVersion(&s, txTo.nVersion);
                ::Serialize(os, txTo.GetVjoinsplit(), nType, nVersion);
                if (txTo.GetVjoinsplit().size() > 0) {
                ::Serialize(s, txTo.joinSplitPubKey, nType, nVersion);
 
                    CTransaction::joinsplit_sig_t nullSig = {};
                    ::Serialize(s, nullSig, nType, nVersion);
                }
            }
        }
        else
        {
            const CScCertificate& certTo = dynamic_cast<const CScCertificate&>(txBaseTo);

            ::Serialize(s, certTo.GetScId(), nType, nVersion);
            ::Serialize(s, certTo.epochNumber, nType, nVersion);
            ::Serialize(s, certTo.quality, nType, nVersion);
            ::Serialize(s, certTo.endEpochBlockHash, nType, nVersion);
            ::Serialize(s, certTo.scProof, nType, nVersion);
            ::Serialize(s, certTo.vFieldElementCertificateField, nType, nVersion);
            ::Serialize(s, certTo.vBitVectorCertificateField, nType, nVersion);

            // Serialize vin
            unsigned int nInputs = fAnyoneCanPay ? 1 : certTo.GetVin().size();
            ::WriteCompactSize(s, nInputs);
            for (unsigned int nInput = 0; nInput < nInputs; nInput++)
                 SerializeInput(s, nInput, nType, nVersion);

            // Serialize vout

            // split bwd transfer and change
            std::vector<CBackwardTransferOut> vbt_ccout_ser;
            // we must not modify vout
            std::vector<CTxOut> vout_ser;
 
            // reading from memory and writing to data stream
            for(int pos = 0; pos < certTo.GetVout().size(); ++pos)
            {
                if (pos < certTo.nFirstBwtPos)
                    vout_ser.push_back(certTo.GetVout()[pos]);
                else
                    vbt_ccout_ser.push_back(CBackwardTransferOut(certTo.GetVout()[pos]));
            }
 
            unsigned int nOutputs = fHashNone ? 0 : (fHashSingle ? nIn+1 : vout_ser.size());
            ::WriteCompactSize(s, nOutputs);
            for (unsigned int nOutput = 0; nOutput < nOutputs; nOutput++)
                 SerializeOutput(s, nOutput, nType, nVersion);
 
            unsigned int voutBtSize = vbt_ccout_ser.size();
            ::WriteCompactSize(s, voutBtSize);
            for (unsigned int nOutput = 0; nOutput < voutBtSize; nOutput++)
                ::Serialize(s, vbt_ccout_ser[nOutput], nType, nVersion);
        }
    }
};

} // anon namespace

uint256 SignatureHash(const CScript& scriptCode, const CTransaction& txTo, unsigned int nIn, int nHashType)
{
    unsigned int nTotalInputs = txTo.IsScVersion() ? txTo.GetVin().size() + txTo.GetVcswCcIn().size() : txTo.GetVin().size();
    if (nIn >= nTotalInputs && nIn != NOT_AN_INPUT) {
        //  nIn out of range
        throw logic_error("input index is out of range");
    }

    // Check for invalid use of SIGHASH_SINGLE
    if ((nHashType & 0x1f) == SIGHASH_SINGLE) {
        if (nIn >= txTo.GetVout().size()) {
            //  nOut out of range
            throw logic_error("no matching output for SIGHASH_SINGLE");
        }
    }

    // Wrapper to serialize only the necessary parts of the transaction being signed
    CTransactionSignatureSerializer txTmp(txTo, scriptCode, nIn, nHashType);

    // Serialize and hash
    CHashWriter ss(SER_GETHASH, 0);
    ss << txTmp << nHashType;
    return ss.GetHash();
}

uint256 SignatureHash(const CScript& scriptCode, const CScCertificate& certTo, unsigned int nIn, int nHashType)
{
    if (nIn >= certTo.GetVin().size() && nIn != NOT_AN_INPUT) {
        //  nIn out of range
        throw logic_error("input index is out of range");
    }

    // Check for invalid use of SIGHASH_SINGLE
    if ((nHashType & 0x1f) == SIGHASH_SINGLE) {
        // consider only the outputs that are not bwt
        unsigned int outSize = certTo.nFirstBwtPos;
        if (nIn >= outSize) {
            //  nOut out of range
            throw logic_error("no matching output for SIGHASH_SINGLE");
        }
    }

    // Wrapper to serialize only the necessary parts of the transaction being signed
    CTransactionSignatureSerializer certTmp(certTo, scriptCode, nIn, nHashType);

    // Serialize and hash
    CHashWriter ss(SER_GETHASH, 0);
    ss << certTmp << nHashType;
    return ss.GetHash();
}

TransactionSignatureChecker::TransactionSignatureChecker(const CTransaction* txToIn,
                                                         unsigned int nInIn,
                                                         const CChain* chainIn):
                                                           txTo(txToIn),
                                                           nIn(nInIn),
                                                           chain(chainIn) {}

TransactionSignatureChecker::TransactionSignatureChecker(const CChain* chainIn): txTo(nullptr), nIn(-1), chain(chainIn) {}

bool TransactionSignatureChecker::VerifySignature(const std::vector<unsigned char>& vchSig, const CPubKey& pubkey, const uint256& sighash) const
{
    return pubkey.Verify(sighash, vchSig);
}

bool TransactionSignatureChecker::CheckSig(const vector<unsigned char>& vchSigIn, const vector<unsigned char>& vchPubKey, const CScript& scriptCode) const
{
    CPubKey pubkey(vchPubKey);
    if (!pubkey.IsValid())
        return false;

    // Hash type is one byte tacked on to the end of the signature
    vector<unsigned char> vchSig(vchSigIn);
    if (vchSig.empty())
        return false;
    int nHashType = vchSig.back();
    vchSig.pop_back();

    uint256 sighash;
    try {
        sighash = SignatureHash(scriptCode, *txTo, nIn, nHashType);
    } catch (const logic_error& ex) {
        return false;
    }

    if (!VerifySignature(vchSig, pubkey, sighash))
        return false;

    return true;
}

bool TransactionSignatureChecker::CheckLockTime(const CScriptNum& nLockTime) const
{
    // There are two times of nLockTime: lock-by-blockheight
    // and lock-by-blocktime, distinguished by whether
    // nLockTime < LOCKTIME_THRESHOLD.
    //
    // We want to compare apples to apples, so fail the script
    // unless the type of nLockTime being tested is the same as
    // the nLockTime in the transaction.
    if (!(
        (txTo->GetLockTime() <  LOCKTIME_THRESHOLD && nLockTime <  LOCKTIME_THRESHOLD) ||
        (txTo->GetLockTime() >= LOCKTIME_THRESHOLD && nLockTime >= LOCKTIME_THRESHOLD)
    ))
        return false;

    // Now that we know we're comparing apples-to-apples, the
    // comparison is a simple numeric one.
    if (nLockTime > (int64_t)txTo->GetLockTime())
        return false;

    // Finally the nLockTime feature can be disabled and thus
    // CHECKLOCKTIMEVERIFY bypassed if every txin has been
    // finalized by setting nSequence to maxint. The
    // transaction would be allowed into the blockchain, making
    // the opcode ineffective.
    //
    // Testing if this vin is not final is sufficient to
    // prevent this condition. Alternatively we could test all
    // inputs, but testing just this input minimizes the data
    // required to prove correct CHECKLOCKTIMEVERIFY execution.
    // Check only regular inputs, skip CSW inputs
    if(nIn < txTo->GetVin().size()) {
        if (txTo->GetVin()[nIn].IsFinal())
            return false;
    }


    return true;
}

bool CheckReplayProtectionData(const CChain* chain, int nHeight, const std::vector<unsigned char>& vchCompareTo)
{
    if (!chain) {
        return false;
    }

    // If the chain doesn't reach the desired height yet, the transaction is non-final
    if (nHeight > chain->Height()) {
        return false;
    }

    // Sufficiently old blocks are always valid
#ifndef BITCOIN_TX
    if (nHeight <= chain->Height() - getCheckBlockAtHeightSafeDepth() ) {
        LogPrint("cbh", "%s: %s():%d - Old block: dont even check [h=%d, chain h=%d, safe depth = %d]\n",
            __FILE__, __func__, __LINE__, nHeight, chain->Height(), getCheckBlockAtHeightSafeDepth() );
        return true;
    }
#else
    // zen-tx does not link all symbols
    if (nHeight <= chain->Height() - 52596) {
        return true;
    }
#endif

    CBlockIndex* pblockindex = (*chain)[nHeight];
    uint256 blockHash = pblockindex->GetBlockHash();
    std::vector<unsigned char> vchBlockHash(blockHash.begin(), blockHash.end());

    if (vchBlockHash.empty() || vchCompareTo.empty()) {
        return false;
    }

    return (vchCompareTo == vchBlockHash);
}

CertificateSignatureChecker::CertificateSignatureChecker(const CScCertificate* certToIn,
                                                         unsigned int nInIn,
                                                         const CChain* chainIn):
                                                           certTo(certToIn),
                                                           nIn(nInIn),
                                                           chain(chainIn) {}

bool CertificateSignatureChecker::VerifySignature(const std::vector<unsigned char>& vchSig, const CPubKey& pubkey, const uint256& sighash) const
{
    return pubkey.Verify(sighash, vchSig);
}

bool CertificateSignatureChecker::CheckSig(const vector<unsigned char>& vchSigIn, const vector<unsigned char>& vchPubKey, const CScript& scriptCode) const
{
    CPubKey pubkey(vchPubKey);
    if (!pubkey.IsValid())
        return false;

    // Hash type is one byte tacked on to the end of the signature
    vector<unsigned char> vchSig(vchSigIn);
    if (vchSig.empty())
        return false;
    int nHashType = vchSig.back();

    if (nHashType != (int)SIGHASH_ALL)
    {
        LogPrintf("%s: %s():%d - ERROR: hash type 0x%x not supported\n", __FILE__, __func__, __LINE__, nHashType);
        return false;
    }

    vchSig.pop_back();

    uint256 sighash;
    try {
        sighash = SignatureHash(scriptCode, *certTo, nIn, nHashType);
    } catch (const logic_error& ex) {
        return false;
    }

    if (!VerifySignature(vchSig, pubkey, sighash))
        return false;

    return true;
}

bool CertificateSignatureChecker::CheckBlockHash(const int32_t nHeight, const std::vector<unsigned char>& vchCompareTo) const
{
    if (!chain) {
        return false;
    }

    // If the chain doesn't reach the desired height yet, the transaction is non-final
    if (nHeight > chain->Height()) {
        return false;
    }

    // Sufficiently old blocks are always valid
#ifndef BITCOIN_TX
    if (nHeight <= chain->Height() - getCheckBlockAtHeightSafeDepth() ) {
        LogPrint("cbh", "%s: %s():%d - Old block: dont even check [h=%d, chain h=%d, safe depth = %d]\n",
            __FILE__, __func__, __LINE__, nHeight, chain->Height(), getCheckBlockAtHeightSafeDepth() );
        return true;
    }
#else
    // zen-tx does not link all symbols
    if (nHeight <= chain->Height() - 52596) {
        return true;
    }
#endif

    CBlockIndex* pblockindex = (*chain)[nHeight];
    uint256 blockHash = pblockindex->GetBlockHash();
    std::vector<unsigned char> vchBlockHash(blockHash.begin(), blockHash.end());

    if (vchBlockHash.empty() || vchCompareTo.empty()) {
        return false;
    }

    return (vchCompareTo == vchBlockHash);
}

bool TransactionSignatureChecker::CheckBlockHash(const int32_t nHeight, const std::vector<unsigned char>& vchCompareTo) const
{
    return CheckReplayProtectionData(chain, nHeight, vchCompareTo);
}

bool VerifyScript(const CScript& scriptSig, const CScript& scriptPubKey, unsigned int flags, const BaseSignatureChecker& checker, ScriptError* serror)
{
    set_error(serror, SCRIPT_ERR_UNKNOWN_ERROR);

    if ((flags & SCRIPT_VERIFY_SIGPUSHONLY) != 0 && !scriptSig.IsPushOnly()) {
        return set_error(serror, SCRIPT_ERR_SIG_PUSHONLY);
    }

    vector<vector<unsigned char> > stack, stackCopy;
    if (!EvalScript(stack, scriptSig, flags, checker, serror))
        // serror is set
        return false;
    if (flags & SCRIPT_VERIFY_P2SH)
        stackCopy = stack;
    if (!EvalScript(stack, scriptPubKey, flags, checker, serror))
        // serror is set
        return false;
    if (stack.empty())
        return set_error(serror, SCRIPT_ERR_EVAL_FALSE);
    if (CastToBool(stack.back()) == false)
        return set_error(serror, SCRIPT_ERR_EVAL_FALSE);

    // Additional validation for spend-to-script-hash transactions:
    if ((flags & SCRIPT_VERIFY_P2SH) && scriptPubKey.IsPayToScriptHash())
    {
        // scriptSig must be literals-only or validation fails
        if (!scriptSig.IsPushOnly())
            return set_error(serror, SCRIPT_ERR_SIG_PUSHONLY);

        // Restore stack.
        swap(stack, stackCopy);

        // stack cannot be empty here, because if it was the
        // P2SH  HASH <> EQUAL  scriptPubKey would be evaluated with
        // an empty stack and the EvalScript above would return false.
        assert(!stack.empty());

        const valtype& pubKeySerialized = stack.back();
        CScript pubKey2(pubKeySerialized.begin(), pubKeySerialized.end());
        popstack(stack);

        if (!EvalScript(stack, pubKey2, flags, checker, serror))
            // serror is set
            return false;
        if (stack.empty())
            return set_error(serror, SCRIPT_ERR_EVAL_FALSE);
        if (!CastToBool(stack.back()))
            return set_error(serror, SCRIPT_ERR_EVAL_FALSE);
    }

    // The CLEANSTACK check is only performed after potential P2SH evaluation,
    // as the non-P2SH evaluation of a P2SH script will obviously not result in
    // a clean stack (the P2SH inputs remain).
    if ((flags & SCRIPT_VERIFY_CLEANSTACK) != 0) {
        // Disallow CLEANSTACK without P2SH, as otherwise a switch CLEANSTACK->P2SH+CLEANSTACK
        // would be possible, which is not a softfork (and P2SH should be one).
        assert((flags & SCRIPT_VERIFY_P2SH) != 0);
        if (stack.size() != 1) {
            return set_error(serror, SCRIPT_ERR_CLEANSTACK);
        }
    }

    return set_success(serror);
}
