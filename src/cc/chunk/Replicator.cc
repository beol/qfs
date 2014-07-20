//---------------------------------------------------------- -*- Mode: C++ -*-
// $Id$
//
// Created 2007/01/17
// Author: Sriram Rao
//         Mike Ovsiannikov -- rework re-replication to protect against
// duplicate requests. Implement chunk recovery.
//
// Copyright 2008-2012 Quantcast Corp.
// Copyright 2007-2008 Kosmix Corp.
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
// \brief Code for dealing with chunk re-replication and recovery.
// The meta server instructs chunk server to obtain a copy of a chunk from a
// source chunk server, or recover chunk by reading other available chunks in
// the RS block and recomputing the missing chunk data. The chunk server reads
// the chunk data from the other chunk server(s) writes chunk replica to disk.
// At the end replication, the destination chunk server notifies the meta
// server.
//
//----------------------------------------------------------------------------

#include "Replicator.h"
#include "ChunkServer.h"
#include "utils.h"
#include "RemoteSyncSM.h"
#include "KfsOps.h"
#include "Logger.h"
#include "BufferManager.h"
#include "DiskIo.h"
#include "ChunkManager.h"
#include "MetaServerSM.h"
#include "ClientManager.h"
#include "ClientThread.h"

#include "common/MsgLogger.h"
#include "common/StdAllocator.h"
#include "common/IntToString.h"

#include "kfsio/KfsCallbackObj.h"
#include "kfsio/NetConnection.h"
#include "kfsio/Globals.h"
#include "kfsio/ClientAuthContext.h"
#include "kfsio/checksum.h"

#include "qcdio/qcstutils.h"

#include "libclient/KfsNetClient.h"
#include "libclient/Reader.h"
#include "libclient/KfsOps.h"

#include <string>
#include <sstream>

namespace KFS
{

using std::string;
using std::ostringstream;
using std::pair;
using std::make_pair;
using std::max;
using std::min;
using KFS::libkfsio::globalNetManager;
using KFS::client::Reader;
using KFS::client::KfsNetClient;

class ReplicatorImpl :
    public KfsCallbackObj,
    public QCRefCountedObj,
    public BufferManager::Client
{
public:
    // Model for doing a chunk replication involves 3 steps:
    //  - First, figure out the size of the chunk.
    //  - Second in a loop:
    //        - read N bytes from the source
    //        - write N bytes to disk
    // - Third, notify the metaserver of the status (0 to mean
    // success, -1 on failure).
    //
    // During replication, the chunk isn't part of the chunkTable data
    // structure that is maintained locally.  This is done for
    // simplifying failure handling: if we die in the midst of
    // replication, upon restart, we will find an incomplete chunk, i.e.
    // chunk with with 0 version in the the dirty directory. Such chunks
    // will be deleted upon restart.
    //
    typedef Replicator::Counters Counters;
    static int GetNumReplications();
    static void CancelAll();
    static void SetParameters(const Properties& props)
    {
        sUseConnectionPoolFlag = props.getValue(
            "chunkServer.replicator.useConnetionPool",
            sUseConnectionPoolFlag ? 1 : 0
        ) != 0;
        sReadSkipDiskVerifyFlag = props.getValue(
            "chunkServer.replicator.readSkipDiskVerify",
            sReadSkipDiskVerifyFlag ? 1 : 0
        ) != 0;
    }
    static void GetCounters(Replicator::Counters& counters);

    ReplicatorImpl(ReplicateChunkOp *op, const RemoteSyncSMPtr &peer);
    void Run();
    // Handle the callback for a size request
    int HandleStartDone(int code, void* data);
    // Handle the callback for a remote read request
    int HandleReadDone(int code, void* data);
    // Handle the callback for a write
    int HandleWriteDone(int code, void* data);
    // When replication done, we write out chunk meta-data; this is
    // the handler that gets called when this event is done.
    int HandleReplicationDone(int code, void* data);
    virtual void Granted(ByteCount byteCount);
    static Counters& Ctrs()
        { return sCounters; };
    static bool GetUseConnectionPoolFlag()
        { return sUseConnectionPoolFlag; }
    static bool CancelChunkReplication(
        kfsChunkId_t chunkId, kfsSeq_t targetVersion);

protected:
    // Inputs from the metaserver
    kfsFileId_t const     mFileId;
    kfsChunkId_t const    mChunkId;
    kfsSeq_t              mChunkVersion;
    // What we obtain from the src from where we download the chunk.
    int64_t               mChunkSize;
    // The op that triggered this replication operation.
    ReplicateChunkOp*     mOwner;
    // What is the offset we are currently reading at
    int64_t               mOffset;
    // Handle to the peer from where we have to get data
    RemoteSyncSMPtr const mPeer;

    GetChunkMetadataOp    mChunkMetadataOp;
    ReadOp                mReadOp;
    WriteOp               mWriteOp;
    // Are we done yet?
    bool                  mDone;
    bool                  mCancelFlag;
    DiskIo::FilePtr       mFileHandle;

    virtual ~ReplicatorImpl();
    // Cleanup...
    void Terminate(int status);
    string GetPeerName() const;
    // Start by sending out a size request
    virtual void Start();
    // Send out a read request to the peer
    virtual void Read();
    virtual void Cancel()
    {
        mCancelFlag = true;
        if (mFileHandle) {
            DiskIo::FilePtr fileH;
            fileH.swap(mFileHandle);
            gChunkManager.ReplicationDone(mChunkId, -ECANCELED, fileH);
        }
        if (IsWaiting()) {
            // Cancel buffers wait, and fail the op.
            CancelRequest();
            Terminate(ECANCELED);
        }
    }
    virtual ByteCount GetBufferBytesRequired() const;

private:
    typedef std::map<
        kfsChunkId_t, ReplicatorImpl*,
        std::less<kfsChunkId_t>,
        StdFastAllocator<
            std::pair<const kfsChunkId_t, ReplicatorImpl*>
        >
    > InFlightReplications;

    static InFlightReplications sInFlightReplications;
    static Counters             sCounters;
    static bool                 sUseConnectionPoolFlag;
    static bool                 sReadSkipDiskVerifyFlag;
private:
    // No copy.
    ReplicatorImpl(const ReplicatorImpl&);
    ReplicatorImpl& operator=(const ReplicatorImpl&);
};

const int kDefaultReplicationReadSize = (int)(
    ((1 << 20) + CHECKSUM_BLOCKSIZE - 1) /
    CHECKSUM_BLOCKSIZE * CHECKSUM_BLOCKSIZE);
ReplicatorImpl::InFlightReplications ReplicatorImpl::sInFlightReplications;
ReplicatorImpl::Counters             ReplicatorImpl::sCounters;
bool ReplicatorImpl::sUseConnectionPoolFlag  = false;
bool ReplicatorImpl::sReadSkipDiskVerifyFlag = true;

int
ReplicatorImpl::GetNumReplications()
{
    return (int)sInFlightReplications.size();
}

void
ReplicatorImpl::CancelAll()
{
    InFlightReplications cancelInFlight;
    cancelInFlight.swap(sInFlightReplications);
    for (InFlightReplications::iterator it = cancelInFlight.begin();
            it != cancelInFlight.end();
            ++it) {
        if (! it->second) {
            continue;
        }
        ReplicatorImpl& cur = *it->second;
        it->second = 0;
        cur.Cancel();
    }
}

bool
ReplicatorImpl::CancelChunkReplication(
    kfsChunkId_t chunkId, kfsSeq_t targetVersion)
{
    InFlightReplications::iterator const it =
        sInFlightReplications.find(chunkId);
    if (it == sInFlightReplications.end() || ! it->second ||
            (0 <= targetVersion && (! it->second->mOwner ||
            ((it->second->mOwner->targetVersion < 0 ?
                it->second->mChunkVersion :
                it->second->mOwner->targetVersion) != targetVersion)))) {
        return false;
    }
    ReplicatorImpl& cur = *it->second;
    sInFlightReplications.erase(it);
    cur.Cancel();
    return true;
}

void ReplicatorImpl::GetCounters(ReplicatorImpl::Counters& counters)
{
    counters = sCounters;
}

ReplicatorImpl::ReplicatorImpl(ReplicateChunkOp *op, const RemoteSyncSMPtr &peer) :
    KfsCallbackObj(),
    QCRefCountedObj(),
    BufferManager::Client(),
    mFileId(op->fid),
    mChunkId(op->chunkId),
    mChunkVersion(op->chunkVersion),
    mOwner(op),
    mOffset(0),
    mPeer(peer),
    mChunkMetadataOp(0),
    mReadOp(0),
    mWriteOp(op->chunkId, op->chunkVersion),
    mDone(false),
    mCancelFlag(false),
    mFileHandle()
{
    mReadOp.chunkId = op->chunkId;
    mReadOp.chunkVersion = op->chunkVersion;
    if (! op->chunkAccess.empty()) {
        mReadOp.requestChunkAccess          = mOwner->chunkAccess.c_str();
        mChunkMetadataOp.requestChunkAccess = mReadOp.requestChunkAccess;
    }
    mReadOp.clnt = this;
    mWriteOp.clnt = this;
    mChunkMetadataOp.clnt = this;
    mWriteOp.Reset();
    mWriteOp.isFromReReplication = true;
    SET_HANDLER(&mReadOp, &ReadOp::HandleReplicatorDone);
    Ctrs().mReplicatorCount++;
}

ReplicatorImpl::~ReplicatorImpl()
{
    InFlightReplications::iterator const it =
        sInFlightReplications.find(mChunkId);
    if (it != sInFlightReplications.end() && it->second == this) {
        sInFlightReplications.erase(it);
    }
    assert(! mOwner && Ctrs().mReplicatorCount > 0);
    Ctrs().mReplicatorCount--;
}

void
ReplicatorImpl::Run()
{
    pair<InFlightReplications::iterator, bool> const ret =
        sInFlightReplications.insert(make_pair(mChunkId, this));
    if (! ret.second) {
        if (! ret.first->second || ret.first->second == this) {
            die("invalid null entry or an attempt to restart replication");
            Terminate(ECANCELED);
            return;
        }
        ReplicatorImpl& other = *ret.first->second;
        KFS_LOG_STREAM_INFO << "replication:"
            " chunk: "   << ret.first->first <<
            " peer: "    << other.GetPeerName() <<
            " offset: "  << other.mOffset <<
            "canceling:" <<
            (other.mCancelFlag ? " already canceled?" : "") <<
            " restarting from"
            " peer: "    << GetPeerName() <<
        KFS_LOG_EOM;
        other.Cancel();
        // Cancel can delete the "other" replicator if it was waiting for
        // buffers for example, and make the iterator invalid.
        pair<InFlightReplications::iterator, bool> const res =
            sInFlightReplications.insert(make_pair(mChunkId, this));
        if (! res.second) {
            assert(ret == res);
            res.first->second = this;
        }
        if (mCancelFlag) {
            Terminate(ECANCELED);
            return;
        }
    }

    const ByteCount kChunkHeaderSize = 16 << 10;
    const ByteCount bufBytes = max(kChunkHeaderSize, GetBufferBytesRequired());
    BufferManager&  bufMgr   = DiskIo::GetBufferManager();
    if (bufMgr.IsOverQuota(*this, bufBytes)) {
        KFS_LOG_STREAM_ERROR << "replication:"
            " chunk: "      << mChunkId <<
            " peer: "       << GetPeerName() <<
            " bytes: "      << bufBytes <<
            " total: "      << GetByteCount() <<
            " over quota: " << bufMgr.GetMaxClientQuota() <<
        KFS_LOG_EOM;
        Terminate(ENOMEM);
        return;
    }
    if (bufMgr.GetForDiskIo(*this, bufBytes)) {
        Start();
        return;
    }
    KFS_LOG_STREAM_INFO << "replication:"
        " chunk: "     << mChunkId <<
        " peer: "      << GetPeerName() <<
        " denined: "   << bufBytes <<
        " waiting for buffers" <<
    KFS_LOG_EOM;
}

ReplicatorImpl::ByteCount
ReplicatorImpl::GetBufferBytesRequired() const
{
    return kDefaultReplicationReadSize;
}

void
ReplicatorImpl::Granted(ByteCount byteCount)
{
    KFS_LOG_STREAM_INFO << "replication:"
        " chunk: "   << mChunkId <<
        " peer: "    << GetPeerName() <<
        " granted: " << byteCount <<
    KFS_LOG_EOM;
    Start();
}

void
ReplicatorImpl::Start()
{
    assert(mPeer);

    mChunkMetadataOp.chunkId           = mChunkId;
    mReadOp.skipVerifyDiskChecksumFlag = sReadSkipDiskVerifyFlag;
    mChunkMetadataOp.readVerifyFlag    = false;
    SET_HANDLER(this, &ReplicatorImpl::HandleStartDone);
    mPeer->Enqueue(&mChunkMetadataOp);
}

int
ReplicatorImpl::HandleStartDone(int code, void* data)
{
    if (mCancelFlag || mChunkMetadataOp.status < 0) {
        if (! mCancelFlag) {
            KFS_LOG_STREAM_INFO << "replication:"
                " chunk: "  << mChunkId <<
                " peer: "   << GetPeerName() <<
                " get chunk meta data failed:"
                " msg: "    << mChunkMetadataOp.statusMsg <<
                " status: " << mChunkMetadataOp.status <<
            KFS_LOG_EOM;
        }
        Terminate(mCancelFlag ? ECANCELED : mChunkMetadataOp.status);
        return 0;
    }
    mChunkSize    = mChunkMetadataOp.chunkSize;
    mChunkVersion = mChunkMetadataOp.chunkVersion;
    if (mChunkSize < 0 || mChunkSize > (int64_t)CHUNKSIZE) {
        KFS_LOG_STREAM_INFO << "replication:"
            " invalid chunk size: " << mChunkSize <<
        KFS_LOG_EOM;
        Terminate(EINVAL);
        return 0;
    }

    assert(! mFileHandle);
    mReadOp.chunkVersion = mChunkVersion;
    // set the version to a value that will never be used; if
    // replication is successful, we then bump up the counter.
    mWriteOp.chunkVersion = 0;
    const bool kIsBeingReplicatedFlag = true;
    const bool kMustExistFlag         = false;
    const int status = gChunkManager.AllocChunk(
        mFileId,
        mChunkId,
        mWriteOp.chunkVersion,
        mOwner->minStorageTier,
        mOwner->minStorageTier,
        kIsBeingReplicatedFlag,
        0,
        kMustExistFlag,
        0, // alloc op
        0 <= mOwner->targetVersion ? mOwner->targetVersion : mChunkVersion,
        &mFileHandle
    );
    if (status < 0) {
        if (status == -EEXIST && mOwner) {
            mOwner->statusMsg =
                "readable chunk with target version already exists";
        }
        Terminate(status);
        return -1;
    }
    if (! mFileHandle) {
        die("replication: invalid null file handle");
        Terminate(-EINVAL);
        return -1;
    }
    KFS_LOG_STREAM_INFO << "replication:"
        " chunk: "  << mChunkId <<
        " peer: "   << GetPeerName() <<
        " starting:"
        " size: "   << mChunkSize <<
    KFS_LOG_EOM;
    Read();
    return 0;
}

void
ReplicatorImpl::Read()
{
    assert(! mCancelFlag && mOwner);
    StRef ref(*this);

    if (mOffset >= mChunkSize) {
        mDone = mOffset == mChunkSize;
        KFS_LOG_STREAM(mDone ?
                MsgLogger::kLogLevelNOTICE :
                MsgLogger::kLogLevelERROR) << "replication:"
            " chunk: "    << mChunkId <<
            " peer: "     << GetPeerName() <<
            (mDone ? " done" : " failed") <<
            " position: " << mOffset <<
            " size: "     << mChunkSize <<
            " "           << mOwner->Show() <<
        KFS_LOG_EOM;
        Terminate(mDone ? 0 : -EIO);
        return;
    }

    if (mOffset % (int)CHECKSUM_BLOCKSIZE != 0) {
        mReadOp.skipVerifyDiskChecksumFlag = false;
    }
    assert(mPeer);
    SET_HANDLER(this, &ReplicatorImpl::HandleReadDone);
    mReadOp.checksum.clear();
    mReadOp.status     = 0;
    mReadOp.offset     = mOffset;
    mReadOp.numBytesIO = 0;
    mReadOp.numBytes   = (int)min(
        mChunkSize - mOffset, int64_t(kDefaultReplicationReadSize));
    mReadOp.dataBuf.Clear();
    mPeer->Enqueue(&mReadOp);
}

int
ReplicatorImpl::HandleReadDone(int code, void* data)
{
    assert(code == EVENT_CMD_DONE && data == &mReadOp);

    if (mCancelFlag) {
        Terminate(ECANCELED);
        return 0;
    }
    const int numRd = mReadOp.dataBuf.BytesConsumable();
    if (mReadOp.status < 0) {
        KFS_LOG_STREAM_INFO << "replication:"
            " chunk: " << mChunkId <<
            " peer: "  << GetPeerName() <<
            " read failed:"
            " error: " << mReadOp.status <<
        KFS_LOG_EOM;
        if (mReadOp.skipVerifyDiskChecksumFlag &&
                mReadOp.status == -EBADCKSUM) {
            KFS_LOG_STREAM_INFO << "replication:"
                " chunk: " << mChunkId <<
                " peer: "  << GetPeerName() <<
                " retrying read:"
                " offset: " << mReadOp.offset <<
                " with disk checksum verify" <<
            KFS_LOG_EOM;
            mReadOp.skipVerifyDiskChecksumFlag = false;
            Read();
            return 0;
        }
    } else if (numRd < (int)mReadOp.numBytes && mOffset + numRd < mChunkSize) {
        KFS_LOG_STREAM_ERROR << "replication:"
            " chunk: "    << mChunkId <<
            " peer: "     << GetPeerName() <<
            " short read:"
            " got: "      << numRd <<
            " expected: " << mReadOp.numBytes <<
        KFS_LOG_EOM;
        mReadOp.status = -EINVAL;
    }
    if (mReadOp.status < 0 || mChunkSize <= mOffset) {
        mDone = mOffset == mChunkSize && 0 <= mReadOp.status;
        Terminate(mDone ? 0 : mReadOp.status);
        return 0;
    }

    const int kChecksumBlockSize = (int)CHECKSUM_BLOCKSIZE;
    if (mOffset % kChecksumBlockSize != 0 ||
            (! mReadOp.checksum.empty() &&
            mReadOp.checksum.size() !=
            (size_t)(numRd + kChecksumBlockSize - 1) / kChecksumBlockSize)) {
        die("replicator: invalid read completion");
        Terminate(EFAULT);
        return 0;
    }
    mWriteOp.Reset();
    mWriteOp.numBytes            = numRd;
    mWriteOp.offset              = mOffset;
    mWriteOp.isFromReReplication = true;
    mWriteOp.dataBuf.Clear();
    if (mReadOp.checksum.empty()) {
        mWriteOp.checksums.clear();
    } else {
        mWriteOp.checksums = mReadOp.checksum;
    }

    // align the writes to checksum boundaries
    bool moveDataFlag = true;
    if (numRd > kChecksumBlockSize) {
        // Chunk manager only handles checksum block aligned writes.
        const int     numBytes = numRd % kChecksumBlockSize;
        const int64_t endPos   = mOffset + numRd;
        assert(numBytes == 0 || endPos == mChunkSize);
        mWriteOp.numBytes = numRd - numBytes;
        if (numBytes > 0 && endPos == mChunkSize) {
            moveDataFlag = false;
            mWriteOp.dataBuf.Move(&mReadOp.dataBuf, mWriteOp.numBytes);
            mReadOp.dataBuf.MakeBuffersFull();
            mReadOp.offset     = mOffset + mWriteOp.numBytes;
            mReadOp.numBytesIO = numBytes;
            mReadOp.numBytes   = numBytes;
            if (! mReadOp.checksum.empty()) {
                mReadOp.checksum.front() = mReadOp.checksum.back();
                mReadOp.checksum.resize(1);
                mWriteOp.checksums.pop_back();
            }
        }
    }
    if (moveDataFlag) {
        mWriteOp.dataBuf.Move(&mReadOp.dataBuf);
    }

    SET_HANDLER(this, &ReplicatorImpl::HandleWriteDone);
    const int status = gChunkManager.WriteChunk(&mWriteOp, &mFileHandle);
    if (status < 0) {
        // abort everything
        Terminate(status);
    }
    return 0;
}

int
ReplicatorImpl::HandleWriteDone(int code, void* data)
{
    assert(
        (code == EVENT_DISK_ERROR) ||
        (code == EVENT_DISK_WROTE) ||
        (code == EVENT_CMD_DONE && data == &mWriteOp)
    );
    StRef ref(*this);
    mWriteOp.diskIo.reset();
    if (mWriteOp.status < 0) {
        KFS_LOG_STREAM_ERROR << "replication:"
            " chunk: "  << mChunkId <<
            " peer:  "  << GetPeerName() <<
            " write failed:"
            " error: "  << mWriteOp.status <<
        KFS_LOG_EOM;
    }
    if (mCancelFlag || mWriteOp.status < 0) {
        Terminate(mCancelFlag ? ECANCELED : mWriteOp.status);
        return 0;
    }
    mOffset += mWriteOp.numBytesIO;
    if (mReadOp.offset == mOffset && ! mReadOp.dataBuf.IsEmpty()) {
        assert(mReadOp.dataBuf.BytesConsumable() < (int)CHECKSUM_BLOCKSIZE);
        // Write the remaining tail.
        HandleReadDone(EVENT_CMD_DONE, &mReadOp);
        return 0;
    }
    Read();
    return 0;
}

void
ReplicatorImpl::Terminate(int status)
{
    int res;
    if (mDone && ! mCancelFlag) {
        KFS_LOG_STREAM_INFO << "replication:"
            " chunk: "   << mChunkId <<
            " version: " << mChunkVersion <<
            " peer: "    << GetPeerName() <<
            " finished"  <<
        KFS_LOG_EOM;
        // The data copy or recovery has completed.
        // Set the version appropriately, and write the meta data.
        SET_HANDLER(this, &ReplicatorImpl::HandleReplicationDone);
        const bool kStableFlag = true;
        res = gChunkManager.ChangeChunkVers(
            mChunkId, mChunkVersion, kStableFlag, this, &mFileHandle);
        if (res == 0) {
            return;
        }
    } else {
        res = status < 0 ? status : (status == 0 ? -1 : -status);
    }
    HandleReplicationDone(EVENT_DISK_ERROR, &res);
}

int
ReplicatorImpl::HandleReplicationDone(int code, void* data)
{
    assert(mOwner);

    const int status = data ? *reinterpret_cast<int*>(data) : 0;
    mOwner->status = status >= 0 ? 0 : status;
    if (status < 0) {
        KFS_LOG_STREAM_ERROR << "replication:" <<
            " chunk: "   << mChunkId <<
            " version: " << mChunkVersion <<
            " peer: "    << GetPeerName() <<
            (mCancelFlag ? " cancelled" : " failed") <<
            " status: "  << status <<
            " " << mOwner->Show() <<
        KFS_LOG_EOM;
    } else {
        const ChunkInfo_t* const ci = gChunkManager.GetChunkInfo(mChunkId);
        KFS_LOG_STREAM_NOTICE << mOwner->Show() <<
            " chunk size: " << (ci ? ci->chunkSize : -1) <<
        KFS_LOG_EOM;
    }
    if (mFileHandle) {
        DiskIo::FilePtr fileH;
        fileH.swap(mFileHandle);
        gChunkManager.ReplicationDone(mChunkId, status, fileH);
    }
    // Notify the owner of completion
    mOwner->chunkVersion = (! mCancelFlag && status >= 0) ? mChunkVersion : -1;
    if (mOwner->status < 0 || mCancelFlag) {
        if (mOwner->location.IsValid()) {
            if (mCancelFlag) {
                Ctrs().mReplicationCanceledCount++;
            } else {
                Ctrs().mReplicationErrorCount++;
            }
        } else {
            if (mCancelFlag) {
                Ctrs().mRecoveryCanceledCount++;
            } else {
                Ctrs().mRecoveryErrorCount++;
            }
        }
    }
    ReplicateChunkOp* const op = mOwner;
    mReadOp.requestChunkAccess          = 0;
    mChunkMetadataOp.requestChunkAccess = 0;
    mOwner = 0;
    UnRef();
    SubmitOpResponse(op);
    return 0;
}

string
ReplicatorImpl::GetPeerName() const
{
    return (mPeer ? mPeer->GetLocation().ToString() : "none");
}

const char* const kRsReadMetaAuthPrefix = "chunkServer.rsReader.auth.";

class RSReplicatorImpl :
    public RSReplicatorEntry,
    public ReplicatorImpl,
    public Reader::Completion
{
public:
    static void SetParameters(const Properties& props)
    {
        const int kChecksumBlockSize = (int)CHECKSUM_BLOCKSIZE;
        sRSReaderMaxRetryCount = props.getValue(
            "chunkServer.rsReader.maxRetryCount",
            sRSReaderMaxRetryCount
        );
        sRSReaderTimeSecBetweenRetries = props.getValue(
            "chunkServer.rsReader.timeSecBetweenRetries",
            sRSReaderTimeSecBetweenRetries
        );
        sRSReaderOpTimeoutSec = props.getValue(
            "chunkServer.rsReader.opTimeoutSec",
            sRSReaderOpTimeoutSec
        );
        sRSReaderIdleTimeoutSec = props.getValue(
            "chunkServer.rsReader.idleTimeoutSec",
            sRSReaderIdleTimeoutSec
        );
        sRSReaderMaxReadSize = (max(1, props.getValue(
            "chunkServer.rsReader.maxReadSize",
            sRSReaderMaxReadSize
        )) + kChecksumBlockSize - 1) / kChecksumBlockSize * kChecksumBlockSize;
        sRSReaderMaxChunkReadSize = props.getValue(
            "chunkServer.rsReader.maxChunkReadSize",
            max(sRSReaderMaxReadSize, sRSReaderMaxChunkReadSize)
        );
        sRSReaderLeaseRetryTimeout = props.getValue(
            "chunkServer.rsReader.leaseRetryTimeout",
            sRSReaderLeaseRetryTimeout
        );
        sRSReaderLeaseWaitTimeout = props.getValue(
            "chunkServer.rsReader.leaseWaitTimeout",
            sRSReaderLeaseWaitTimeout
        );
        sRSReaderMetaMaxRetryCount  = props.getValue(
            "chunkServer.rsReader.meta.maxRetryCount",
            sRSReaderMetaMaxRetryCount
        );
        sRSReaderMetaTimeSecBetweenRetries = props.getValue(
            "chunkServer.rsReader.meta.timeSecBetweenRetries",
            sRSReaderMetaTimeSecBetweenRetries
        );
        sRSReaderMetaOpTimeoutSec = props.getValue(
            "chunkServer.rsReader.meta.opTimeoutSec",
            sRSReaderMetaOpTimeoutSec
        );
        sRSReaderMetaIdleTimeoutSec = props.getValue(
            "chunkServer.rsReader.meta.idleTimeoutSec",
            sRSReaderMetaIdleTimeoutSec
        );
        sRSReaderMetaResetConnectionOnOpTimeoutFlag = props.getValue(
            "chunkServer.rsReader.meta.idleTimeoutSec",
            sRSReaderMetaResetConnectionOnOpTimeoutFlag ? 1 : 0
        ) != 0;
        sRSReaderMaxRecoverChunkSize = props.getValue(
            "chunkServer.rsReader.maxRecoverChunkSize",
            sRSReaderMaxRecoverChunkSize
        );
        sRSReaderPanicOnInvalidChunkFlag = props.getValue(
            "chunkServer.rsReader.panicOnInvalidChunk",
            sRSReaderPanicOnInvalidChunkFlag ? 1 : 0) != 0;
        sMaxRecoveryThreads = props.getValue(
            "chunkServer.rsReader.maxRecoveryThreads",
            sMaxRecoveryThreads
        );
        if (0 < props.copyWithPrefix(kRsReadMetaAuthPrefix, sAuthParams)) {
            sAuthUpdateCount++;
        }
    }
    static RSReplicatorImpl* Create(
        ReplicateChunkOp* op,
        const char*       sessionToken,
        int               sessionTokenLen,
        const char*       sessionKey,
        int               sessionKeyLen)
    {
        const bool authFlag = 0 < sessionTokenLen && 0 < sessionKeyLen;
        if (authFlag) {
            static const Properties::String kPskKeyIdParam(
                kRsReadMetaAuthPrefix + string("psk.keyId"));
            static const Properties::String kPskKeyParam(
                kRsReadMetaAuthPrefix + string("psk.key"));
            static Properties::String tmp;
            tmp.Copy(sessionToken, sessionTokenLen);
            const Properties::String* val =
                sAuthParams.getValue(kPskKeyIdParam);
            if (! val || *val != tmp) {
                sAuthParams.setValue(kPskKeyIdParam, tmp);
                sAuthUpdateCount++;
            }
            tmp.Copy(sessionKey, sessionKeyLen);
            val = sAuthParams.getValue(kPskKeyParam);
            if (! val || *val != tmp) {
                sAuthParams.setValue(kPskKeyParam, tmp);
                sAuthUpdateCount++;
            }
        }
        ClientThread*                   clientThread    = 0;
        const MetaServers::Entry* const entry           =
            GetMetaserver(authFlag, op, clientThread);
        if (! entry || ! entry->mMeta ||
                (entry->mAuth != 0) != authFlag ||
                entry->mAuth != entry->mMeta->GetAuthContext()) {
            const char* const msg = "recovery: invalid meta server entry";
            die(msg);
            op->statusMsg = msg;
            op->status    = -EFAULT;
            return 0;
        }
        return new RSReplicatorImpl(
            op,
            (authFlag && entry->mAuthUpdateCount != sAuthUpdateCount) ?
                const_cast<uint64_t*>(&entry->mAuthUpdateCount) : 0,
            clientThread,
            *entry->mMeta
        );
    }
    static void Shutdown()
    {
        CancelAll();
        StopMetaServers();
    }

private:
    enum State
    {
        kNone   = 0,
        kStart  = 1,
        kRead   = 2
    };
    typedef ClientThread::StMutexLocker StMutexLocker;

    State                mState;
    KfsNetClient&        mMetaServer;
    uint64_t* const      mAuthUpdateCount;
    Reader               mReader;
    IOBuffer             mReadTail;
    const ServerLocation mLocation;
    const int            mReadSize;
    bool                 mReadInFlightFlag;
    bool                 mPendingCloseFlag;
    bool                 mPendingCancelFlag;

    RSReplicatorImpl(
        ReplicateChunkOp* op,
        uint64_t*         authUpdateCount,
        ClientThread*     clientThread,
        KfsNetClient&     metaServer)
        : RSReplicatorEntry(clientThread),
          ReplicatorImpl(op, RemoteSyncSMPtr()),
          Reader::Completion(),
          mState(kNone),
          mMetaServer(metaServer),
          mAuthUpdateCount(authUpdateCount),
          mReader(
            metaServer,
            this,
            sRSReaderMaxRetryCount,
            sRSReaderTimeSecBetweenRetries,
            sRSReaderOpTimeoutSec,
            sRSReaderIdleTimeoutSec,
            sRSReaderMaxChunkReadSize,
            sRSReaderLeaseRetryTimeout,
            sRSReaderLeaseWaitTimeout,
            MakeLogPrefix(mChunkId),
            GetSeqNum()
        ),
        mReadTail(),
        mLocation(gMetaServerSM.GetLocation().hostname, op->location.port),
        mReadSize(GetReadSize(*op)),
        mReadInFlightFlag(false),
        mPendingCloseFlag(false),
        mPendingCancelFlag(false)
    {
        assert(mReadSize % IOBufferData::GetDefaultBufferSize() == 0);
        mReadOp.clnt = 0; // Should not queue read op.
    }
    virtual ~RSReplicatorImpl()
    {
        KFS_LOG_STREAM_DEBUG << "~RSReplicatorImpl"
            " chunk: " << mChunkId <<
        KFS_LOG_EOM;
        mReader.Register(0);
        mReader.Shutdown();
    }
    virtual void Cancel()
    {
        if (mPendingCancelFlag) {
            return;
        }
        mPendingCancelFlag = true;
        Enqueue(mState);
    }
    virtual void Read()
    {
        assert(mState == kNone);
        if (mPendingCancelFlag) {
            KFS_LOG_STREAM_DEBUG <<
                "recovery: ignoring read, cancel pending"
                " state: "   << mState <<
                " pending: " << IsPending() <<
            KFS_LOG_EOM;
            return;
        }
        Enqueue(kRead);
    }
    virtual ByteCount GetBufferBytesRequired() const
    {
        return (mReadSize * (mOwner ? mOwner->numStripes + 1 : 0));
    }
    void Enqueue(State inState)
    {
        if (mPendingCancelFlag) {
            if (mState != inState) {
                die("recovery: invalid cancel enqueue");
                return;
            }
        } else if (mState != kNone) {
            ostringstream os;
            os << "recovery: invalid state transtion"
                " from: "    << (int)mState <<
                " to: "      << (int)inState <<
                " pending: " << IsPending()
            ;
            die(os.str());
            return;
        } else {
            mState = inState;
        }
        if (mClientThreadPtr &&
                mClientThreadPtr != ClientThread::GetCurrentClientThreadPtr()) {
            RSReplicatorEntry::Enqueue();
        } else {
            Handle();
        }
    }
    virtual void Handle()
    {
        // Pending cancel flag check is racy here (mutex isn't acquired).
        // Handle cancel acquires the mutex and checks if the entry is still
        // queued, and ignores cancellation requests until drains the queue
        // completely.
        if (mPendingCancelFlag) {
            HandleCancel();
            return;
        }
        switch (mState) {
            case kStart:
                HandleStart();
                break;
            case kRead:
                HandleRead();
                break;
            default:
                die("recovery: invalid state");
                break;
        }
    }
    void HandleCompletion(void* data)
    {
        if (mPendingCancelFlag) {
            return; // Ignore completion.
        }
        mState = kNone;
        if (data == &mChunkMetadataOp) {
            HandleStartDone(EVENT_CMD_DONE, data);
        } else {
            HandleReadDone(EVENT_CMD_DONE, data);
        }
    }
    virtual void Start()
    {
        assert(mOwner && mOwner->status == 0 && mState == kNone);
        if (mPendingCancelFlag) {
            KFS_LOG_STREAM_DEBUG <<
                "recovery: ignoring start, cancel pending"
                " state: "   << mState <<
                " pending: " << IsPending() <<
            KFS_LOG_EOM;
            return;
        }
        mChunkMetadataOp.chunkSize         = CHUNKSIZE;
        mChunkMetadataOp.chunkVersion      = mOwner->chunkVersion;
        mChunkMetadataOp.status            = 0;
        mChunkMetadataOp.statusMsg.clear();
        mReadOp.status                     = 0;
        mReadOp.statusMsg.clear();
        mReadOp.numBytes                   = 0;
        mReadOp.skipVerifyDiskChecksumFlag = false;
        if (! mLocation.IsValid()) {
            mChunkMetadataOp.status    = -EINVAL;
            mChunkMetadataOp.statusMsg =
                "invalid meta server location: " + mLocation.ToString() +
                " or authentication";
            HandleCompletion(&mChunkMetadataOp);
            return;
        }
        Enqueue(kStart);
    }
    virtual void Done(
        Reader&           inReader,
        int               inStatusCode,
        Reader::Offset    inOffset,
        Reader::Offset    inSize,
        IOBuffer*         inBufferPtr,
        Reader::RequestId inRequestId)
    {
        StRef ref(*this);

        if (&inReader != &mReader || (inBufferPtr &&
                (inRequestId.mPtr != this ||
                    inOffset < 0 ||
                    inSize > (Reader::Offset)mReadOp.numBytes ||
                    ! mReadInFlightFlag))) {
            die("recovery: invalid read completion");
            mReadOp.status = -EINVAL;
        }
        if (mPendingCloseFlag) {
            if (! mReader.IsActive()) {
                KFS_LOG_STREAM_DEBUG << "recovery:"
                    " chunk: " << mChunkId <<
                    " chunk reader closed" <<
                KFS_LOG_EOM;
                mPendingCloseFlag = false;
                UnRef();
            }
            return;
        }
        if (! mReadInFlightFlag) {
            if (mReadOp.status >= 0 && inStatusCode < 0) {
                mReadOp.status = inStatusCode;
            }
            return;
        }
        mReadInFlightFlag = false;
        if (mReadOp.status != 0 || (! inBufferPtr && inStatusCode == 0)) {
            return;
        }
        mReadOp.checksum.clear();
        mReadOp.status = inStatusCode;
        const bool readOkFlag  = mReadOp.status == 0 && inBufferPtr;
        const int  pendingSize = readOkFlag ?
            mReadTail.BytesConsumable() + inBufferPtr->BytesConsumable() : 0;
        if (readOkFlag) {
            const bool endOfChunk =
                mReadSize > inBufferPtr->BytesConsumable() ||
                mOffset + mReadTail.BytesConsumable() + mReadSize >= mChunkSize;
            IOBuffer& buf = mReadOp.dataBuf;
            buf.Clear();
            if (endOfChunk) {
                buf.Move(&mReadTail);
                buf.Move(inBufferPtr);
                mReadOp.numBytes   = buf.BytesConsumable();
                mReadOp.numBytesIO = mReadOp.numBytes;
                mChunkSize = mOffset + mReadOp.numBytesIO;
                mReader.Close();
                if (mReader.IsActive()) {
                    mPendingCloseFlag = true;
                    Ref();
                }
            } else {
                const int kChecksumBlockSize = (int)CHECKSUM_BLOCKSIZE;
                int nmv = (mReadTail.BytesConsumable() +
                    inBufferPtr->BytesConsumable()) /
                    kChecksumBlockSize * kChecksumBlockSize;
                if (nmv <= 0) {
                    mReadTail.Move(inBufferPtr);
                    HandleRead();
                    return;
                }
                nmv -= buf.Move(&mReadTail, nmv);
                buf.Move(inBufferPtr, nmv);
                mReadTail.Move(inBufferPtr);
                mReadOp.numBytes   = buf.BytesConsumable();
                mReadOp.numBytesIO = mReadOp.numBytes;
            }
            if (0 < mReadOp.numBytes && ! buf.IsEmpty() &&
                        mReadOp.offset   % (int)CHECKSUM_BLOCKSIZE == 0 &&
                        mReadOp.numBytes % (int)CHECKSUM_BLOCKSIZE == 0) {
                mReadOp.checksum = ComputeChecksums(&buf, mReadOp.numBytes);
            }
        }
        StMutexLocker lock(mClientThreadPtr);
        if (readOkFlag &&
                sRSReaderMaxRecoverChunkSize < mOffset + pendingSize) {
            ostringstream os;
            os << " recovery:"
                " file: "   << mFileId  <<
                " chunk: "  << mChunkId <<
                " pos: "    << mOffset  <<
                " + "       << mReadTail.BytesConsumable() <<
                " rdsize: " << inBufferPtr->BytesConsumable() <<
                " exceeds " << sRSReaderMaxRecoverChunkSize;
            const string msg = os.str();
            die(msg);
        }
        if (! mOwner) {
            return;
        }
        if (mOwner->chunkOffset + mOffset != inOffset) {
            die("recovery: invalid read completion");
            mReadOp.status = -EINVAL;
        }
        if (! readOkFlag &&
                (inStatusCode < 0 && inBufferPtr &&
                ! inBufferPtr->IsEmpty())) {
            mOwner->invalidStripeIdx.clear();
            string&       str = mOwner->invalidStripeIdx;
            // Report invalid stripes.
            const int     ns = mOwner->numStripes + mOwner->numRecoveryStripes;
            int           n  = 0;
            while (! inBufferPtr->IsEmpty()) {
                if (n >= ns) {
                    die("recovery: completion: invalid number of bad stripes");
                    n = 0;
                    break;
                }
                int          idx          = -1;
                kfsChunkId_t chunkId      = -1;
                int64_t      chunkVersion = -1;
                ReadVal(*inBufferPtr, idx);
                ReadVal(*inBufferPtr, chunkId);
                ReadVal(*inBufferPtr, chunkVersion);
                if (idx < 0 || idx >= ns) {
                    die("recovery: completion: invalid bad stripe index");
                    n = 0;
                    break;
                }
                if (0 < n) {
                    str += ' ';
                }
                AppendDecIntToString(str, idx);
                str += ' ';
                AppendDecIntToString(str, chunkId);
                str += ' ';
                AppendDecIntToString(str, chunkVersion);
                n++;
            }
            if (n > 0) {
                KFS_LOG_STREAM_ERROR << "recovery: "
                    " status: "          << inStatusCode <<
                    " invalid stripes: " << mOwner->invalidStripeIdx <<
                    " file size: "       << mOwner->fileSize <<
                KFS_LOG_EOM;
                if (sRSReaderPanicOnInvalidChunkFlag && 0 < mOwner->fileSize) {
                    const string msg = "recovery: invalid chunk(s) detected: " +
                        mOwner->invalidStripeIdx;
                    die(msg);
                }
            }
        }
        HandleCompletion(&mReadOp);
    }
    void HandleCancel()
    {
        StRef ref(*this);

        const int prevRef = GetRefCount();
        mReader.Unregister(this);
        mReader.Shutdown();
        if (GetRefCount() < prevRef) {
            return; // Unwind.
        }
        StMutexLocker lock(mClientThreadPtr);
        if (IsPending()) {
            // Drain pending queue, cancel can be queued multiple times, due to
            // race in between en-queue and de-queue.
            return;
        }
        ReplicatorImpl::Cancel();
        if (GetRefCount() < prevRef || mState == kNone) {
            return; // Unwind.
        }
        if (mReadInFlightFlag) {
            assert(mOwner);
            mReadInFlightFlag = false;
            mReadOp.status = -ETIMEDOUT;
            HandleReadDone(EVENT_CMD_DONE, &mReadOp);
            return;
        }
        assert(mState == kStart);
        mChunkMetadataOp.status = -ETIMEDOUT;
        HandleStartDone(EVENT_CMD_DONE, &mChunkMetadataOp);
    }
    void HandleStart()
    {
        assert(! mCancelFlag && mOwner && mOwner->status == 0 &&
            ! mReadInFlightFlag && mLocation.IsValid());

        if (mAuthUpdateCount) {
            ClientAuthContext* const authContext = mMetaServer.GetAuthContext();
            if (authContext) {
                // Acquire lock here to serialize access to sAuthParams.
                StMutexLocker lock(mClientThreadPtr);
                if (*mAuthUpdateCount != sAuthUpdateCount) {
                    KFS_LOG_STREAM_DEBUG <<
                        "recovery: updating authentication context" <<
                        " update count: " << *mAuthUpdateCount <<
                        " / " << sAuthUpdateCount <<
                    KFS_LOG_EOM;
                    ClientAuthContext* const kOtherCtx   = 0;
                    const bool               kVerifyFlag = false;
                    mChunkMetadataOp.status = authContext->SetParameters(
                        kRsReadMetaAuthPrefix,
                        sAuthParams,
                        kOtherCtx,
                        &mChunkMetadataOp.statusMsg,
                        kVerifyFlag
                    );
                    *mAuthUpdateCount = sAuthUpdateCount;
                }
            } else {
                die("recovery: invalid null authentication context");
                mChunkMetadataOp.status = -EFAULT;
            }
        }
        const ServerLocation& loc = mMetaServer.GetServerLocation();
        if (mLocation != loc) {
            if (loc.IsValid()) {
                KFS_LOG_STREAM_INFO <<
                    "recovery:"
                    " meta server client address has changed"
                    " from: " << loc <<
                    " to: "   << mLocation <<
                KFS_LOG_EOM;
            }
            const bool kCancelPendingOpsFlag = true;
            bool       kForceConnectFlag     = false;
            if (! mMetaServer.SetServer(
                    mLocation,
                    kCancelPendingOpsFlag,
                    &mChunkMetadataOp.statusMsg,
                    kForceConnectFlag)) {
                mChunkMetadataOp.status = -EHOSTUNREACH;
            }
        }
        if (0 <= mChunkMetadataOp.status) {
            const bool kSkipHolesFlag                 = true;
            const bool kUseDefaultBufferAllocatorFlag = true;
            mChunkMetadataOp.status = mReader.Open(
                mFileId,
                mOwner->pathName.c_str(),
                mOwner->fileSize,
                mOwner->striperType,
                mOwner->stripeSize,
                mOwner->numStripes,
                mOwner->numRecoveryStripes,
                kSkipHolesFlag,
                kUseDefaultBufferAllocatorFlag,
                mOwner->chunkOffset
            );
        }
        StMutexLocker lock(mClientThreadPtr);
        HandleCompletion(&mChunkMetadataOp);
    }
    void HandleRead()
    {
        assert(! mCancelFlag && mOwner && ! mReadInFlightFlag);
        if (mOffset >= mChunkSize || mReadOp.status < 0) {
            StMutexLocker lock(mClientThreadPtr);
            HandleCompletion(&mReadOp);
            return;
        }

        StRef ref(*this);
        mReadOp.status     = 0;
        mReadOp.numBytes   = mReadSize;
        mReadOp.numBytesIO = 0;
        mReadOp.offset     = mOffset;
        mReadOp.dataBuf.Clear();
        Reader::RequestId reqId = Reader::RequestId();
        reqId.mPtr = this;
        mReadInFlightFlag = true;
        IOBuffer buf;
        const int status = mReader.Read(
            buf,
            mReadSize,
            mOffset + mReadTail.BytesConsumable(),
            reqId
        );
        if (status != 0 && mReadInFlightFlag) {
            mReadInFlightFlag = false;
            mReadOp.status = status;
            StMutexLocker lock(mClientThreadPtr);
            HandleCompletion(&mReadOp);
        }
    }
    template<typename T> static void ReadVal(IOBuffer& buf, T& val)
    {
        const int len = (int)sizeof(val);
        if (buf.Consume(buf.CopyOut(
                reinterpret_cast<char*>(&val), len)) != len) {
            die("invalid buffer size");
        }
    }
    struct AddExtraClientHeaders
    {
        AddExtraClientHeaders(const char* hdrs)
        {
            client::KfsOp::AddExtraRequestHeaders(hdrs);
            client::KfsOp::AddDefaultRequestHeaders(
                kKfsUserRoot, kKfsGroupRoot);
        }
    };
    static void StopMetaServers()
    {
        ClientThread* clientThread = 0;
        GetMetaserver(false, 0, clientThread);
    }
    class MetaServers
    {
    public:
        class Entry
        {
        public:
            Entry()
                : mMeta(0),
                  mAuth(0),
                  mAuthUpdateCount(0)
                {}
            KfsNetClient*      mMeta;
            ClientAuthContext* mAuth;
            uint64_t           mAuthUpdateCount;
        };
        MetaServers(const Entry* inServers, int inCount)
            : mServers(inServers),
              mCount(inCount)
            {}
        ~MetaServers()
        {
            MetaServers::Stop();
            for (int i = 0; i < mCount; i++) {
                delete mServers[i].mMeta;
                delete mServers[i].mAuth;
            }
            delete [] mServers;
        }
        void Stop() const
        {
            for (int i = 0; i < mCount; i++) {
                mServers[i].mMeta->Stop();
                mServers[i].mMeta->SetAuthContext(0);
                if (mServers[i].mAuth) {
                    mServers[i].mAuth->Clear();
                }
            }
        }
        const Entry* const mServers;
        const int          mCount;
    };
    static const MetaServers::Entry* CreateMetaServers(
        int inMaxCount, bool authFlag)
    {
        MetaServers::Entry* const ret = new MetaServers::Entry[inMaxCount];
        char                      buf[sizeof(int) * 3 + 4 + 4];
        char* const               end = buf + sizeof(buf) / sizeof(buf[0]);
        for (int i = 0; i < inMaxCount; i++) {
            char* name = end;
            *--name = 0;
            name = IntToDecString(i, name);
            if (authFlag) {
                *--name = 'A';
            }
            *--name = 'R';
            *--name = 'S';
            *--name = 'R';
            ret[i].mMeta = new KfsNetClient(
                i == 0 ?
                    globalNetManager() :
                    gClientManager.GetClientThread(i - 1)->GetNetManager(),
                string(), // inHost
                0,        // inPort
                sRSReaderMetaMaxRetryCount,
                sRSReaderMetaTimeSecBetweenRetries,
                sRSReaderMetaOpTimeoutSec,
                sRSReaderMetaIdleTimeoutSec,
                GetRandomSeq(),
                name,
                sRSReaderMetaResetConnectionOnOpTimeoutFlag
            );
            ret[i].mAuth = authFlag ? new ClientAuthContext() : 0;
            ret[i].mMeta->SetAuthContext(ret[i].mAuth);
        }
        return ret;
    }
    static const MetaServers::Entry* GetMetaserver(
        bool              authFlag,
        ReplicateChunkOp* op,
        ClientThread*&    clientThread)
    {
        static int sLastIdx = -1;
        if (sLastIdx < 0) {
            if (! op) {
                clientThread = 0;
                return 0;
            }
            sLastIdx = 0;
        }
        static const AddExtraClientHeaders sAddHdrs("From-chunk-server: 1\r\n");
        static const int                   sMaxCount(
            max(0, gClientManager.GetClientThreadCount()) + 1);
        static const MetaServers           sMetaServers(
            CreateMetaServers(sMaxCount, false), sMaxCount);
        static const MetaServers           sMetaServersAuth(
            CreateMetaServers(sMaxCount,  true), sMaxCount);
        if (! op) {
            sMetaServers.Stop();
            sMetaServersAuth.Stop();
            clientThread = 0;
            sLastIdx  = -1;
            return 0;
        }
        if (min(sMaxRecoveryThreads, sMaxCount) <= ++sLastIdx) {
            sLastIdx = (sMaxCount <= 1 || sMaxRecoveryThreads <= 0) ? 0 : 1;
        }
        clientThread = sLastIdx <= 0 ? 0 :
            gClientManager.GetClientThread(sLastIdx - 1);
        return ((authFlag ? sMetaServersAuth : sMetaServers
            ).mServers + sLastIdx);
    }
    static const char* MakeLogPrefix(kfsChunkId_t chunkId)
    {
        const size_t kSize = sizeof(kfsChunkId_t) * 3 + 5;
        static char  buf[kSize + 1];
        buf[kSize] = 0;
        char* pref = IntToDecString(chunkId, buf + kSize);
        *--pref = ' ';
        *--pref = ':';
        *--pref = 'R';
        *--pref = 'C';
        return pref;
    }
    static kfsSeq_t GetSeqNum()
    {
        static kfsSeq_t sInitialSeqNum = GetRandomSeq();
        static uint32_t sNextRand      = (uint32_t)sInitialSeqNum;
        sNextRand = sNextRand * 1103515245 + 12345;
        sInitialSeqNum += 100000 + ((uint32_t)(sNextRand / 65536) % 32768);
        return sInitialSeqNum;
    }
    static int GetReadSize(const ReplicateChunkOp& op)
    {
        // Align read on checksum block boundary, and align on stripe size,
        // if possible.
        const int kChecksumBlockSize = (int)CHECKSUM_BLOCKSIZE;
        const int kIoBufferSize      = IOBufferData::GetDefaultBufferSize();
        assert(
            sRSReaderMaxReadSize >= kChecksumBlockSize &&
            op.stripeSize > 0 &&
            sRSReaderMaxReadSize % kChecksumBlockSize == 0 &&
            kChecksumBlockSize % kIoBufferSize == 0
        );
        const int size = max(kChecksumBlockSize, (int)min(
            int64_t(sRSReaderMaxReadSize),
            (DiskIo::GetBufferManager().GetMaxClientQuota() /
                max(1, op.numStripes + 1)) /
            kChecksumBlockSize * kChecksumBlockSize)
        );
        if (size <= op.stripeSize) {
            KFS_LOG_STREAM_DEBUG << "recovery:"
                " large stripe: " << op.stripeSize <<
                " read size: "    << size <<
            KFS_LOG_EOM;
            return size;
        }
        int lcm = GetLcm(kChecksumBlockSize, op.stripeSize);
        if (lcm > size) {
            lcm = GetLcm(kIoBufferSize, op.stripeSize);
            if (lcm > size) {
                KFS_LOG_STREAM_WARN << "recovery:"
                    "invalid read parameters:"
                    " max read size:  " << sRSReaderMaxReadSize <<
                    " io buffer size: " << kIoBufferSize <<
                    " stripe size: "    << op.stripeSize <<
                    " set read size: "  << lcm <<
                KFS_LOG_EOM;
                return lcm;
            }
        }
        return (size / lcm * lcm);
    }
    static int GetGcd(int nl, int nr)
    {
        int a = nl;
        int b = nr;
        while (b != 0) {
            const int t = b;
            b = a % b;
            a = t;
        }
        return a;
    }
    static int GetLcm(int nl, int nr)
        { return ((nl == 0 || nr == 0) ? 0 : nl / GetGcd(nl, nr) * nr); }

    static int        sRSReaderMaxRetryCount;
    static int        sRSReaderTimeSecBetweenRetries;
    static int        sRSReaderOpTimeoutSec;
    static int        sRSReaderIdleTimeoutSec;
    static int        sRSReaderMaxChunkReadSize;
    static int        sRSReaderMaxReadSize;
    static int        sRSReaderLeaseRetryTimeout;
    static int        sRSReaderLeaseWaitTimeout;
    static int        sRSReaderMetaMaxRetryCount;
    static int        sRSReaderMetaTimeSecBetweenRetries;
    static int        sRSReaderMetaOpTimeoutSec;
    static int        sRSReaderMetaIdleTimeoutSec;
    static int        sRSReaderMaxRecoverChunkSize;
    static int        sMaxRecoveryThreads;
    static bool       sRSReaderMetaResetConnectionOnOpTimeoutFlag;
    static bool       sRSReaderPanicOnInvalidChunkFlag;
    static uint64_t   sAuthUpdateCount;
    static Properties sAuthParams;
private:
    // No copy.
    RSReplicatorImpl(const RSReplicatorImpl&);
    RSReplicatorImpl& operator=(const RSReplicatorImpl&);
};
int  RSReplicatorImpl::sRSReaderMaxRetryCount                      = 3;
int  RSReplicatorImpl::sRSReaderTimeSecBetweenRetries              = 10;
int  RSReplicatorImpl::sRSReaderOpTimeoutSec                       = 30;
int  RSReplicatorImpl::sRSReaderIdleTimeoutSec                     = 5 * 30;
int  RSReplicatorImpl::sRSReaderMaxReadSize                        =
    kDefaultReplicationReadSize;
int  RSReplicatorImpl::sRSReaderMaxChunkReadSize                   =
    max(kDefaultReplicationReadSize, 1 << 20);
int  RSReplicatorImpl::sRSReaderLeaseRetryTimeout                  = 3;
int  RSReplicatorImpl::sRSReaderLeaseWaitTimeout                   = 30;
int  RSReplicatorImpl::sRSReaderMetaMaxRetryCount                  = 2;
int  RSReplicatorImpl::sRSReaderMetaTimeSecBetweenRetries          = 10;
int  RSReplicatorImpl::sRSReaderMetaOpTimeoutSec                   = 4 * 60;
int  RSReplicatorImpl::sRSReaderMetaIdleTimeoutSec                 = 5 * 60;
int  RSReplicatorImpl::sMaxRecoveryThreads                         = 16;
bool RSReplicatorImpl::sRSReaderMetaResetConnectionOnOpTimeoutFlag = true;
int  RSReplicatorImpl::sRSReaderMaxRecoverChunkSize                =
    (int)CHUNKSIZE;
bool RSReplicatorImpl::sRSReaderPanicOnInvalidChunkFlag            = false;
uint64_t   RSReplicatorImpl::sAuthUpdateCount = 0;
Properties RSReplicatorImpl::sAuthParams;

int
Replicator::GetNumReplications()
{
    return ReplicatorImpl::GetNumReplications();
}

void
Replicator::CancelAll()
{
    ReplicatorImpl::CancelAll();
}

bool
Replicator::Cancel(kfsChunkId_t chunkId, kfsSeq_t targetVersion)
{
    return ReplicatorImpl::CancelChunkReplication(chunkId, targetVersion);
}

void
Replicator::Shutdown()
{
    ReplicatorImpl::CancelAll();
    RSReplicatorImpl::Shutdown();
}

void
Replicator::SetParameters(const Properties& props)
{
    ReplicatorImpl::SetParameters(props);
    RSReplicatorImpl::SetParameters(props);
}

void
Replicator::GetCounters(Replicator::Counters& counters)
{
    ReplicatorImpl::GetCounters(counters);
}

void
Replicator::Run(ReplicateChunkOp* op)
{
    assert(op && ! gClientManager.GetCurrentClientThreadPtr());
    KFS_LOG_STREAM_DEBUG << op->Show() << KFS_LOG_EOM;

    const char*       p = op->chunkServerAccess.GetPtr();
    const char* const e = p + op->chunkServerAccess.GetSize();
    while (p < e && (*p & 0xFF) <= ' ') {
        ++p;
    }
    const char* const token = p;
    while (p < e && ' ' < (*p & 0xFF)) {
        ++p;
    }
    const int tokenLen = (int)(p - token);
    while (p < e && (*p & 0xFF) <= ' ') {
        ++p;
    }
    const char* const key = p;
    while (p < e && ' ' < (*p & 0xFF)) {
        ++p;
    }
    const int keyLen = (int)(p - key);
    if ((0 < keyLen) != (0 < tokenLen)) {
        op->status    = -EINVAL;
        op->statusMsg = "malformed chunk access header value";
        if (op->location.IsValid()) {
            ReplicatorImpl::Ctrs().mReplicationErrorCount++;
        } else {
            ReplicatorImpl::Ctrs().mRecoveryErrorCount++;
        }
        KFS_LOG_STREAM_ERROR <<
            (op->location.IsValid() ? "replication: " : "recovery: ") <<
            op->statusMsg <<
            " " << op->Show() <<
        KFS_LOG_EOM;
        SubmitOpResponse(op);
        return;
    }
    ReplicatorImpl* impl = 0;
    if (op->location.IsValid()) {
        ReplicatorImpl::Ctrs().mReplicationCount++;
        RemoteSyncSMPtr peer;
        const bool kKeyIsNotEncryptedFlag = true;
        if (ReplicatorImpl::GetUseConnectionPoolFlag()) {
            const bool kConnectFlag = true;
            peer = gChunkServer.FindServer(
                op->location,
                kConnectFlag,
                token,
                tokenLen,
                key,
                keyLen,
                kKeyIsNotEncryptedFlag,
                op->allowCSClearTextFlag,
                op->status,
                op->statusMsg
            );
            if (op->status < 0) {
                peer.reset();
            }
        } else {
            const bool theConnectFlag              =
                gClientManager.GetMutexPtr() == 0;
            const bool theForceUseClientThreadFlag = ! theConnectFlag;
            peer = RemoteSyncSM::Create(
                op->location,
                token,
                tokenLen,
                key,
                keyLen,
                kKeyIsNotEncryptedFlag,
                op->allowCSClearTextFlag,
                op->status,
                op->statusMsg,
                theConnectFlag,
                theForceUseClientThreadFlag
            );
            if (peer && op->status < 0) {
                peer.reset();
            }
        }
        if (peer) {
            impl = new ReplicatorImpl(op, peer);
        } else {
            KFS_LOG_STREAM_ERROR << "replication:"
                "unable to find peer: " << op->location.ToString() <<
                " " << op->Show() <<
            KFS_LOG_EOM;
            if (0 <= op->status) {
                op->status = -EHOSTUNREACH;
            }
            ReplicatorImpl::Ctrs().mReplicationErrorCount++;
        }
    } else {
        ReplicatorImpl::Ctrs().mRecoveryCount++;
        if (op->chunkOffset < 0 ||
                op->chunkOffset % int64_t(CHUNKSIZE) != 0 ||
                op->striperType != KFS_STRIPED_FILE_TYPE_RS ||
                op->numStripes <= 0 ||
                op->numRecoveryStripes <= 0 ||
                op->stripeSize < KFS_MIN_STRIPE_SIZE ||
                op->stripeSize > KFS_MAX_STRIPE_SIZE ||
                CHUNKSIZE % op->stripeSize != 0 ||
                op->stripeSize % KFS_STRIPE_ALIGNMENT != 0 ||
                op->location.port <= 0) {
            op->status = -EINVAL;
            KFS_LOG_STREAM_ERROR << "replication:"
                "invalid request: " << op->Show() <<
            KFS_LOG_EOM;
            ReplicatorImpl::Ctrs().mRecoveryErrorCount++;
        } else {
            impl = RSReplicatorImpl::Create(op, token, tokenLen, key, keyLen);
        }
    }
    if (impl) {
        impl->Ref();
        impl->Run();
    } else {
        SubmitOpResponse(op);
    }
}

} // namespace KFS
