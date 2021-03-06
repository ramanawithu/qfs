//---------------------------------------------------------- -*- Mode: C++ -*-
// $Id$
//
// Created 2016/08/09
//
// Author: Mike Ovsiannikov
//
// Copyright 2009 Quantcast Corp.
//
// This file is part of Kosmos File System (KFS).
//
// Licensed under the Apache License, Version 2.0
// (the "License"); you may not use this file except in compliance with
// the License. You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
// implied. See the License for the specific language governing
// permissions and limitations under the License.
//
// List files with object store blocks missing.
//
//----------------------------------------------------------------------------

#include "kfstree.h"
#include "Checkpoint.h"
#include "Restorer.h"
#include "Replay.h"
#include "MetaRequest.h"
#include "Logger.h"
#include "util.h"

#include "common/MdStream.h"
#include "common/MsgLogger.h"
#include "common/RequestParser.h"
#include "common/Properties.h"

#include "kfsio/blockname.h"
#include "kfsio/ClientAuthContext.h"
#include "kfsio/NetManager.h"
#include "kfsio/SslFilter.h"

#include "libclient/KfsClient.h"
#include "libclient/KfsNetClient.h"
#include "libclient/KfsOps.h"

#include <iostream>
#include <string>
#include <vector>

#include <boost/static_assert.hpp>
#include <boost/dynamic_bitset.hpp>

#include <signal.h>

namespace KFS
{
using std::cout;
using std::cin;
using std::cerr;
using std::string;
using std::vector;
using boost::dynamic_bitset;

using namespace client;

class ObjStoreFsck : public KfsNetClient::OpOwner
{
public:
    static int Run(
        int    inArgCnt,
        char** inArgaPtr)
    {
        MsgLogger::Init(0, MsgLogger::kLogLevelERROR);
        signal(SIGPIPE, SIG_IGN);
        libkfsio::InitGlobals();
        MdStream::Init();
        int theStatus;
        const SslFilter::Error theError = SslFilter::Initialize();
        if (0 != theError) {
            KFS_LOG_STREAM_FATAL <<
                "failed to initialize ssl status: " << theError <<
                " " << SslFilter::GetErrorMsg(theError) <<
            KFS_LOG_EOM;
            theStatus = -1;
        } else {
            ObjStoreFsck theFsck;
            theStatus = theFsck.RunSelf(inArgCnt, inArgaPtr);
        }
        SslFilter::Cleanup();
        MdStream::Cleanup();
        MsgLogger::Stop();
        return (0 == theStatus ? 0 : 1);
    }
private:
    typedef dynamic_bitset<> BlocksBitmap;

    ClientAuthContext mAuthContext;
    NetManager        mNetManager;
    KfsNetClient      mKfsNetClient;
    LeafIter          mLeafIter;
    bool              mQueryFlag;
    int64_t           mLostCount;
    int               mError;
    int               mInFlightCnt;
    int               mMaxInFlightCnt;

    ObjStoreFsck()
    : OpOwner(),
      mAuthContext(),
      mNetManager(),
      mKfsNetClient(
        mNetManager,
        string(),     // inHost
        0,            // inPort
        3,            // inMaxRetryCount
        10,           // inTimeSecBetweenRetries
        5  * 60,      // inOpTimeoutSec
        30 * 60,      // inIdleTimeoutSec
        InitialSeq()  // inInitialSeqNum,
      ),
      mLeafIter(0, 0),
      mQueryFlag(false),
      mLostCount(0),
      mError(0),
      mInFlightCnt(0),
      mMaxInFlightCnt(1 << 10)
        { mKfsNetClient.SetAuthContext(&mAuthContext); }
    virtual ~ObjStoreFsck()
    {
        if (0 != mInFlightCnt) {
            panic("~ObjStoreFsck non 0 in flight count");
        }
        mInFlightCnt = -1000;
    }
    virtual void OpDone(
        KfsOp*    inOpPtr,
        bool      inCanceledFlag,
        IOBuffer* inBufferPtr)
    {
        if (! inOpPtr || inBufferPtr) {
            panic("invalid null op completion");
            return;
        }
        KFS_LOG_STREAM_DEBUG <<
            "done:"
            " status: "    << inOpPtr->status <<
            (inOpPtr->statusMsg.empty() ? "" : " ")
                << inOpPtr->statusMsg <<
            " "            << inOpPtr->Show() <<
            " in flight: " << mInFlightCnt <<
        KFS_LOG_EOM;
        if (! mQueryFlag) {
            if (0 != mInFlightCnt) {
                panic("invalid non zero in flight count");
            }
            mKfsNetClient.Stop();
            mNetManager.Shutdown();
            return;
        }
        mInFlightCnt--;
        if (inCanceledFlag) {
            delete inOpPtr;
            return;
        }
        GetPathNameOp& theOp = *static_cast<GetPathNameOp*>(inOpPtr);
        if (inOpPtr->status < 0) {
            if (-ENOENT != theOp.status) {
                KFS_LOG_STREAM_ERROR <<
                    "file id: " << theOp.fid << ": " <<
                    (theOp.statusMsg.empty() ?
                        ErrorCodeToStr(theOp.status) :
                        theOp.statusMsg
                    ) <<
                KFS_LOG_EOM;
                if (0 == mError) {
                    mError = -inOpPtr->status;
                }
            }
        } else {
            ReportLost(theOp.pathname);
        }
        Next(&theOp);
        if (mInFlightCnt <= 0) {
            mKfsNetClient.Stop();
            mNetManager.Shutdown();
        }
    }
    void ReportLost(
        const string& inPathName)
    {
        mLostCount++;
        cout << inPathName << "\n";
    }
    static int64_t InitialSeq()
    {
        int64_t theRet = 0;
        CryptoKeys::PseudoRand(&theRet, sizeof(theRet));
        return ((theRet < 0 ? -theRet : theRet) >> 1);
    }
    static int RestoreCheckpoint(
        const string& inLockFileName)
    {
        if (! inLockFileName.empty()) {
            acquire_lockfile(inLockFileName, 10);
        }
        Restorer theRestorer;
        return (theRestorer.rebuild(LASTCP) ? 0 : -EIO);
    }
    static bool HasBitmapSet(
        const MetaFattr& theFattr)
    {
        const int64_t kBits = 8 * sizeof(theFattr.subcount1);
        return (kBits * (int64_t)CHUNKSIZE <= theFattr.nextChunkOffset());
    }
    static BlocksBitmap* GetBitmapPtr(
        const MetaFattr& inFattr)
    {
        BOOST_STATIC_ASSERT(sizeof(BlocksBitmap*) <= sizeof(inFattr.subcount1));
        char* const kNullPtr = 0;
        return reinterpret_cast<BlocksBitmap*>(kNullPtr + inFattr.chunkcount());
    }
    static void SetBitmapPtr(
        MetaFattr&    inFattr,
        BlocksBitmap* inPtr)
    {
        const char* const kNullPtr = 0;
        inFattr.chunkcount() = reinterpret_cast<const char*>(inPtr) - kNullPtr;
    }
    int SetParameters(
        const ServerLocation& inMetaLocation,
        const char*           inConfigFileNamePtr)
    {
        Properties  theProperties;
        int         theStatus    = 0;
        const char* theConfigPtr = inConfigFileNamePtr;
        if (inConfigFileNamePtr) {
            const char kDelimeter = '=';
            theStatus = theProperties.loadProperties(theConfigPtr, kDelimeter);
        } else {
            theStatus = KfsClient::LoadProperties(
                inMetaLocation.hostname.c_str(),
                inMetaLocation.port,
                0,
                theProperties,
                theConfigPtr
            );
        }
        if (theStatus == 0 && theConfigPtr) {
            const bool         kVerifyFlag  = true;
            ClientAuthContext* kOtherCtxPtr = 0;
            string*            kErrMsgPtr   = 0;
            theStatus = mAuthContext.SetParameters(
                "client.auth.",
                theProperties,
                kOtherCtxPtr,
                kErrMsgPtr,
                kVerifyFlag
            );
        }
        return theStatus;
    }
    int Start(
        const ServerLocation inLocation)
    {
        if (0 < mInFlightCnt) {
            panic("invalid start invocation with ops in flight");
            return -EINVAL;
        }
        mLostCount = 0;
        mError     = 0;
        mQueryFlag = false;
        if (! inLocation.IsValid()) {
            return 0;
        }
        mNetManager.UpdateTimeNow();
        if (! mKfsNetClient.SetServer(inLocation)) {
            return -EHOSTUNREACH;
        }
        GetPathNameOp theOp(0, ROOTFID, -1);
        if (! mKfsNetClient.Enqueue(&theOp, this)) {
            KFS_LOG_STREAM_FATAL << "failed to enqueue op: " <<
                theOp.Show() <<
            KFS_LOG_EOM;
            return -EFAULT;
        }
        const bool     kWakeupAndCleanupFlag = false;
        QCMutex* const kNullMutexPtr         = 0;
        mNetManager.MainLoop(kNullMutexPtr, kWakeupAndCleanupFlag);
        mKfsNetClient.Cancel();
        mKfsNetClient.Stop();
        if (theOp.status < 0) {
            KFS_LOG_STREAM_ERROR <<
                (theOp.statusMsg.empty() ?
                    ErrorCodeToStr(theOp.status) :
                    theOp.statusMsg
                ) <<
            KFS_LOG_EOM;
        }
        mQueryFlag = 0 == theOp.status;
        return theOp.status;
    }
    void Next(
        GetPathNameOp* inOpPtr)
    {
        GetPathNameOp* theOpPtr = inOpPtr;
        for (const Meta* theNPtr = 0;
                mLeafIter.parent() && (theNPtr = mLeafIter.current());
                mLeafIter.next()) {
            if (KFS_FATTR != theNPtr->metaType()) {
                continue;
            }
            const MetaFattr& theFattr = *static_cast<const MetaFattr*>(theNPtr);
            if (KFS_FILE != theFattr.type || 0 != theFattr.numReplicas ||
                   theFattr.filesize <= 0) {
                continue;
            }
            chunkOff_t theMissingIdx = 0;
            if (HasBitmapSet(theFattr)) {
                const BlocksBitmap* const thePtr = GetBitmapPtr(theFattr);
                if (thePtr) {
                    for (theMissingIdx = 0;
                            theMissingIdx < (chunkOff_t)thePtr->size() &&
                                (*thePtr)[theMissingIdx];
                            ++theMissingIdx)
                        {}
                    // Do not do cleanup to speedup (reduce CPU utilization).
                    // thePtr->chunkcount() = 0;
                    // delete thePtr;
                }
            } else {
                const int64_t theBits = theFattr.chunkcount();
                const int64_t theEnd  = theFattr.nextChunkOffset() / CHUNKSIZE;
                int64_t       theBit  = 1;
                for (theMissingIdx = 0;
                        theMissingIdx <= theEnd && 0 != (theBits & theBit);
                        theMissingIdx++, theBit <<= 1)
                    {}
            }
            if (theMissingIdx * (chunkOff_t)CHUNKSIZE < theFattr.filesize) {
                if (mQueryFlag) {
                    if (theOpPtr) {
                        theOpPtr->fid     = theFattr.id();
                        theOpPtr->chunkId = -1;
                        theOpPtr->status  = 0;
                        theOpPtr->statusMsg.clear();
                        theOpPtr->pathname.clear();
                    } else if (mMaxInFlightCnt <= mInFlightCnt) {
                        break;
                    } else {
                        theOpPtr = new GetPathNameOp(0, theFattr.id(), -1);
                    }
                    mInFlightCnt++;
                    if (mKfsNetClient.Enqueue(theOpPtr, this)) {
                        theOpPtr = 0;
                    } else {
                        KFS_LOG_STREAM_ERROR <<
                            "enqueue error, id: " << theFattr.id() <<
                        KFS_LOG_EOM;
                        if (0 == mError) {
                            mError = -EFAULT;
                        }
                        mInFlightCnt--;
                        break;
                    }
                } else {
                    ReportLost(metatree.getPathname(&theFattr));
                }
            }
        }
        delete theOpPtr;
    }
    int RunSelf(
        int    inArgCnt,
        char** inArgaPtr);
private:
    ObjStoreFsck(
        const ObjStoreFsck& inFsck);
    ObjStoreFsck& operator=(
        const ObjStoreFsck& inFsck);
};


    int
ObjStoreFsck::RunSelf(
    int    inArgCnt,
    char** inArgaPtr)
{
    int                 theOpt;
    string              theCpDir;
    string              theLockFile;
    ServerLocation      theMetaServer;
    string              theLogDir;
    const char*         theConfigFileNamePtr = 0;
    MsgLogger::LogLevel theLogLevel          = MsgLogger::kLogLevelINFO;
    MsgLogger::LogLevel theLogLevelNoFile    = MsgLogger::kLogLevelDEBUG;
    int                 theStatus            = 0;
    bool                theHelpFlag          = false;
    bool                theReplayLastLogFlag = false;
    const char*         thePtr;

    while ((theOpt = getopt(inArgCnt, inArgaPtr, "vhail:c:L:s:p:f:x:")) != -1) {
        switch (theOpt) {
            case 'a':
                theReplayLastLogFlag = true;
                break;
            case 'L':
                theLockFile = optarg;
                break;
            case 'l':
                theLogDir = optarg;
                break;
            case 'c':
                theCpDir = optarg;
                break;
            case 's':
                theMetaServer.hostname = optarg;
                break;
            case 'i':
                theLogLevelNoFile = MsgLogger::kLogLevelINFO;
                break;
            case 'p':
                thePtr = optarg;
                if (! DecIntParser::Parse(
                        thePtr, strlen(thePtr), theMetaServer.port)) {
                    theMetaServer.port = -1;
                }
                break;
            case 'v':
                theLogLevel = MsgLogger::kLogLevelDEBUG;
                break;
            case 'h':
                theHelpFlag = true;
                break;
            case 'f':
                theConfigFileNamePtr = optarg;
                break;
            case 'x':
                thePtr = optarg;
                if (! DecIntParser::Parse(
                        thePtr, strlen(thePtr), mMaxInFlightCnt)) {
                    mMaxInFlightCnt = -1;
                }
                break;
                break;
            default:
                theStatus = -EINVAL;
                break;
        }
    }
    if (theHelpFlag || 0 != theStatus ||
            (mMaxInFlightCnt <= 0 && theMetaServer.IsValid()) ||
            (! theMetaServer.hostname.empty() && ! theMetaServer.IsValid())) {
        cerr <<
            "Usage: " << inArgaPtr[0] << "\n"
            "[-h <help>]\n"
            "[-v verbose]\n"
            "[-L <lock file>] default: no lock file\n"
            "[-l <transaction log directory>] default: kfslog\n"
            "[-c <checkpoint directory>] default: kfscp\n"
            "[-f <client configuration file>] default: none\n"
            "[-a replay last log segment] default: don't replay last segment\n"
            "[-x <max pipelined get info meta ops>] default: 1024\n"
            "[-s <meta server host>]\n"
            "[-p <meta server port>]\n"
            "\n"
            "Loads checkpoint, replays transaction logs, then"
            " reads object store block keys from standard in, one key per line,"
            " and outputs \"lost\" file names on standard out (files with keys"
            " that were not present in standard in), if any."
            "\n\n"
            "Note that the list of object store block keys must be"
            " more recent than checkpoint, and transaction logs, and valid"
            " meta server host and port must be specified in order for"
            " this work correctly (no false positives) if the file system is"
            " \"live\" / being modified."
            "\n\n"
            "In other words, the correct procedure to check \"live\" file system"
            " is to copy / save checkpoint, and transaction logs, then create"
            " list of object store blocks, then run this tool."
            "\n"
        ;
        return 1;
    }
    MsgLogger::SetLevel(theLogLevel);
    if (! theCpDir.empty()) {
        checkpointer_setup_paths(theCpDir);
    }
    if (! theLogDir.empty()) {
        logger_setup_paths(theLogDir);
    }
    if (0 == (theStatus = SetParameters(
                theMetaServer, theConfigFileNamePtr))) {
        theStatus = Start(theMetaServer);
    }
    if (0 == theStatus &&
            (theStatus = RestoreCheckpoint(theLockFile)) == 0 &&
            (theStatus = replayer.playLogs(theReplayLastLogFlag)) == 0) {
        if (! mQueryFlag) {
            // Setup back pointers, to get file names retrival working.
            metatree.setUpdatePathSpaceUsage(true);
            metatree.enableFidToPathname();
        }
        const int64_t theFileSystemId = metatree.GetFsId();
        string        theExpectedKey;
        string        theBlockKey;
        string        theFsIdSuffix;
        theExpectedKey.reserve(256);
        theBlockKey.reserve(256);
        int64_t theKeysCount = 0;
        while (getline(cin, theBlockKey)) {
            KFS_LOG_STREAM_DEBUG <<
                "key: " << theBlockKey <<
            KFS_LOG_EOM;
            const char*       thePtr    = theBlockKey.data();
            const char* const theEndPtr = thePtr + theBlockKey.size();
            if (theEndPtr <= thePtr) {
                continue;
            }
            const int kSeparator = '.';
            thePtr = reinterpret_cast<const char*>(
                memchr(thePtr, kSeparator, theEndPtr - thePtr));
            theKeysCount++;
            fid_t theFid     = -1;
            seq_t theVersion = 0;
            if (! thePtr ||
                    theEndPtr <= ++thePtr ||
                    ! DecIntParser::Parse(thePtr, theEndPtr - thePtr, theFid) ||
                    theFid < 0 ||
                    theEndPtr <= thePtr ||
                    kSeparator != (0xFF & *thePtr) ||
                    theEndPtr <= ++thePtr ||
                    ! DecIntParser::Parse(
                        thePtr, theEndPtr - thePtr, theVersion) ||
                    0 <= theVersion ||
                    theEndPtr <= thePtr ||
                    kSeparator != (0xFF & *thePtr)) {
                KFS_LOG_STREAM_ERROR <<
                    theBlockKey << ": malformed object store block key" <<
                KFS_LOG_EOM;
                continue;
            }
            theExpectedKey.clear();
            if (! AppendChunkFileNameOrObjectStoreBlockKey(
                    theExpectedKey,
                    theFileSystemId,
                    theFid,
                    theFid,
                    theVersion,
                    theFsIdSuffix)) {
                panic("block name generation failure");
                continue;
            }
            if (theExpectedKey != theBlockKey) {
                KFS_LOG_STREAM_ERROR <<
                    theBlockKey    << ": invalid object store block key"
                    " expected: "  << theExpectedKey <<
                KFS_LOG_EOM;
                continue;
            }
            MetaFattr* const theFattrPtr = metatree.getFattr(theFid);
            if (! theFattrPtr) {
                KFS_LOG_STREAM(theLogLevelNoFile) <<
                    theBlockKey << ": invalid key: no such file" <<
                KFS_LOG_EOM;
                continue;
            }
            if (KFS_FILE != theFattrPtr->type) {
                KFS_LOG_STREAM_ERROR <<
                    theBlockKey << ": invalid key:"
                    " attribute type: " << theFattrPtr->type <<
                KFS_LOG_EOM;
                continue;
            }
            if (0 != theFattrPtr->numReplicas) {
                KFS_LOG_STREAM_ERROR <<
                    theBlockKey << ": invalid key:"
                    " replication: " << theFattrPtr->numReplicas <<
                KFS_LOG_EOM;
                continue;
            }
            if (theFattrPtr->filesize <= 0) {
                KFS_LOG_STREAM_DEBUG <<
                    theBlockKey << ": skipping 0 size file" <<
                KFS_LOG_EOM;
                continue;
            }
            const chunkOff_t thePos = -theVersion - 1 - theFattrPtr->minSTier;
            if (thePos < 0 || 0 != (thePos % (chunkOff_t)CHUNKSIZE)) {
                KFS_LOG_STREAM_ERROR <<
                    theBlockKey << ": invalid key:"
                    " position: " << thePos <<
                    " tier: "     << theFattrPtr->minSTier <<
                    " / "         << theFattrPtr->maxSTier <<
                KFS_LOG_EOM;
                continue;
            }
            if (theFattrPtr->nextChunkOffset() < thePos) {
                KFS_LOG_STREAM(
                        theFattrPtr->nextChunkOffset() +
                            (chunkOff_t)CHUNKSIZE < thePos ?
                        MsgLogger::kLogLevelERROR :
                        MsgLogger::kLogLevelDEBUG) <<
                    theBlockKey << ": block past last file block"
                        " position: "   << thePos <<
                        " last block: " << theFattrPtr->nextChunkOffset()  <<
                KFS_LOG_EOM;
                continue;
            }
            // Chunk count must be 0 for object store files. Use this field to
            // store bitmap of the blocks that are present in the input. If the
            // file has more blocks that fits into the chunk count field, then
            // allocate bit vector and store pointer to it.
            const size_t theIdx = thePos / CHUNKSIZE;
            if (HasBitmapSet(*theFattrPtr)) {
                BlocksBitmap* thePtr = GetBitmapPtr(*theFattrPtr);
                if (! thePtr) {
                    thePtr = new BlocksBitmap(
                        1 + theFattrPtr->nextChunkOffset() / CHUNKSIZE);
                    SetBitmapPtr(*theFattrPtr, thePtr);
                } else if ((*thePtr)[theIdx]) {
                    KFS_LOG_STREAM_DEBUG <<
                        theBlockKey << ": duplicate input key" <<
                    KFS_LOG_EOM;
                    continue;
                }
                (*thePtr)[theIdx] = true;
            } else {
                const int64_t theBit = int64_t(1) << theIdx;
                if (0 != (theFattrPtr->chunkcount() & theBit)) {
                    KFS_LOG_STREAM_DEBUG <<
                        theBlockKey << ": duplicate input key" <<
                    KFS_LOG_EOM;
                    continue;
                }
                theFattrPtr->chunkcount() |= theBit;
            }
        }
        KFS_LOG_STREAM_INFO
            "read keys: "    << theKeysCount <<
            " total:"
            " files: "       << GetNumFiles() << 
            " directories: " << GetNumDirs() <<
        KFS_LOG_EOM;
        // Traverse leaf nodes and query the the status for files with missing
        // blocks.
        mLeafIter.reset(metatree.firstLeaf(), 0);
        Next(0);
        if (0 < mInFlightCnt) {
            mNetManager.MainLoop();
        }
        theStatus = mError;
    }
    if (0 != theStatus) {
        KFS_LOG_STREAM_ERROR <<
            ErrorCodeToStr(theStatus) <<
        KFS_LOG_EOM;
    } else {
        KFS_LOG_STREAM_INFO <<
            "lost files: " << mLostCount <<
        KFS_LOG_EOM;
    }
    return (0 == theStatus ? (0 < mLostCount ? -EINVAL : 0) : theStatus);
}

} // namespace KFS

int
main(int argc, char **argv)
{
    return KFS::ObjStoreFsck::Run(argc, argv);
}
