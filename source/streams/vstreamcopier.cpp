/*
Copyright c1997-2014 Trygve Isaacson. All rights reserved.
This file is part of the Code Vault version 4.1
http://www.bombaydigital.com/
License: MIT. See LICENSE.md in the Vault top level directory.
*/

/** @file */

#include "vstreamcopier.h"
#include "vstream.h"
#include "viostream.h"

VStreamCopier::VStreamCopier()
    : mChunkSize(0)
    , mFrom(NULL)
    , mTo(NULL)
    , mNumBytesCopied(0)
    {
}

VStreamCopier::VStreamCopier(int chunkSize, VStream* from, VStream* to)
    : mChunkSize(chunkSize)
    , mFrom(from)
    , mTo(to)
    , mNumBytesCopied(0)
    {
}

VStreamCopier::VStreamCopier(int chunkSize, VIOStream* from, VIOStream* to)
    : mChunkSize(chunkSize)
    , mFrom(&from->getRawStream())
    , mTo(&to->getRawStream())
    , mNumBytesCopied(0)
    {
}

VStreamCopier::VStreamCopier(int chunkSize, VStream* from, VIOStream* to)
    : mChunkSize(chunkSize)
    , mFrom(from)
    , mTo(&to->getRawStream())
    , mNumBytesCopied(0)
    {
}

VStreamCopier::VStreamCopier(int chunkSize, VIOStream* from, VStream* to)
    : mChunkSize(chunkSize)
    , mFrom(&from->getRawStream())
    , mTo(to)
    , mNumBytesCopied(0)
    {
}

void VStreamCopier::init(int chunkSize, VStream* from, VStream* to) {
    mChunkSize = chunkSize;
    mFrom = from;
    mTo = to;
    mNumBytesCopied = 0;
}

void VStreamCopier::init(int chunkSize, VIOStream* from, VIOStream* to) {
    mChunkSize = chunkSize;
    mFrom = &from->getRawStream();
    mTo = &to->getRawStream();
    mNumBytesCopied = 0;
}

void VStreamCopier::init(int chunkSize, VStream* from, VIOStream* to) {
    mChunkSize = chunkSize;
    mFrom = from;
    mTo = &to->getRawStream();
    mNumBytesCopied = 0;
}

void VStreamCopier::init(int chunkSize, VIOStream* from, VStream* to) {
    mChunkSize = chunkSize;
    mFrom = &from->getRawStream();
    mTo = to;
    mNumBytesCopied = 0;
}

bool VStreamCopier::copyChunk() {
    Vs64    numBytesToCopy = mChunkSize;
    Vs64    theNumBytesCopied = 0;

    if ((mFrom != NULL) && (mTo != NULL)) {
        theNumBytesCopied = VStream::streamCopy(*mFrom, *mTo, numBytesToCopy);
    }

    mNumBytesCopied += theNumBytesCopied;

    return (theNumBytesCopied == numBytesToCopy);
}

Vs64 VStreamCopier::numBytesCopied() const {
    return mNumBytesCopied;
}

