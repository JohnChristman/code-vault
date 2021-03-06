/*
Copyright c1997-2014 Trygve Isaacson. All rights reserved.
This file is part of the Code Vault version 4.1
http://www.bombaydigital.com/
License: MIT. See LICENSE.md in the Vault top level directory.
*/

#include "vclientsession.h"
#include "vtypes_internal.h"

#include "vmutexlocker.h"
#include "vserver.h"
#include "vlogger.h"
#include "vmessage.h"
#include "vmessageinputthread.h"
#include "vmessageoutputthread.h"
#include "vsocket.h"
#include "vbento.h"

// VClientSession --------------------------------------------------------------

VClientSession::VClientSession(const VString& sessionBaseName, VServer* server, const VString& clientType, VSocket* socket, VMessageInputThread* inputThread, VMessageOutputThread* outputThread, const VDuration& standbyTimeLimit, Vs64 maxQueueDataSize)
    : VEnableSharedFromThis<VClientSession>()
    , mName(sessionBaseName)
    , mLoggerName(VSTRING_ARGS("vault.messages.VClientSession.%s.%s", sessionBaseName.chars(), VLogger::getCleansedLoggerName(socket->getHostIPAddress()).chars()))
    , mMutex(VString::EMPTY()/*name will be set in body*/)
    , mServer(server)
    , mClientType(clientType)
    , mClientIP(socket->getHostIPAddress())
    , mClientPort(socket->getPortNumber())
    , mClientAddress()
    , mInputThread(inputThread)
    , mOutputThread(outputThread)
    , mIsShuttingDown(false)
    , mStartupStandbyQueue()
    , mStandbyStartTime(VInstant::NEVER_OCCURRED())
    , mStandbyTimeLimit(standbyTimeLimit)
    , mMaxClientQueueDataSize(maxQueueDataSize)
    , mSocket(socket)
    , mSocketStream(socket, "VClientSession") // FIXME: find a way to get the IP address here or to set in ctor
    , mIOStream(mSocketStream)
    {
    mClientAddress.format("%s:%d", mClientIP.chars(), mClientPort);
    mName.format("%s:%s:%d", sessionBaseName.chars(), mClientIP.chars(), mClientPort);
    mMutex.setName(VSTRING_FORMAT("VClientSession[%s]::mMutex", mName.chars()));

    if (mServer == NULL) {
        VString message(VSTRING_ARGS("[%s] VClientSession: No server specified.", this->getClientAddress().chars()));
        VLOGGER_NAMED_ERROR(mLoggerName, message);
        throw VStackTraceException(message);
    }
}

VClientSession::~VClientSession() {
    try {
        this->_releaseQueuedClientMessages();
    } catch (...) {}

    mInputThread = NULL;
    mOutputThread = NULL;

    delete mSocket;
}

void VClientSession::initIOThreads() {
    if (mInputThread != NULL) {
        mInputThread->attachSession(shared_from_this());
        mInputThread->start();
    }

    if (mOutputThread != NULL) {
        mOutputThread->attachSession(shared_from_this());
        mOutputThread->start();
    }
}

void VClientSession::shutdown(VThread* callingThread) {
    VMutexLocker locker(&mMutex, VSTRING_FORMAT("[%s]VClientSession::shutdown() %s", this->getName().chars(), (callingThread == NULL ? "" : callingThread->getName().chars())));

    mIsShuttingDown = true;

    if (callingThread == NULL) {
        VLOGGER_NAMED_DEBUG(mLoggerName, VSTRING_FORMAT("[%s] VClientSession::shutdown: Server requested shutdown of VClientSession@0x%08X.", this->getName().chars(), this));
    }

    if (mInputThread != NULL) {
        if (callingThread == mInputThread) {
            mInputThread = NULL;
            VLOGGER_NAMED_DEBUG(mLoggerName, VSTRING_FORMAT("[%s] VClientSession::shutdown: Input Thread [%s] requested shutdown of VClientSession@0x%08X.", this->getName().chars(), callingThread->getName().chars(), this));
        } else {
            mInputThread->stop();
        }
    }

    if (mOutputThread != NULL) {
        if (callingThread == mOutputThread) {
            mOutputThread = NULL;
            VLOGGER_NAMED_DEBUG(mLoggerName, VSTRING_FORMAT("[%s] VClientSession::shutdown: Output Thread [%s] requested shutdown of VClientSession@0x%08X.", this->getName().chars(), callingThread->getName().chars(), this));
        } else {
            mOutputThread->stop();
        }
    }

    // Remove this session from the server's lists of active sessions,
    // so that it can be garbage collected.
    locker.unlock(); // Must release mMutex to avoid possibility of deadlock with a thread that could be posting broadcast right now, which has server lock, needs our lock. removeClientSession may need server lock. Deadlock.
    mServer->removeClientSession(shared_from_this());
}

void VClientSession::postOutputMessage(VMessagePtr message, bool isForBroadcast) {
    VMutexLocker locker(&mMutex, VSTRING_FORMAT("[%s]VClientSession::postOutputMessage()", this->getName().chars())); // protect the mStartupStandbyQueue during queue operations

    // Don't post if client is doing a disconnect:
    if (mIsShuttingDown || this->isClientGoingOffline()) {
        VLOGGER_NAMED_WARN(mLoggerName, VSTRING_FORMAT("VClientSession::postOutputMessage: NOT posting message@0x%08X to going-offline session [%s], presumably in process of session shutdown.", message.get(), mClientAddress.chars()));
        return;
    }

    if (isForBroadcast && ! this->isClientOnline()) {
        // This branch is entered only for posting to an offline session (e.g. a session still starting up
        // that is not ready to receive normal "posted" messages.
        // We either post to the session's standby queue, or if we hit a limit we start killing the session.

        VInstant now;

        if (mStandbyStartTime == VInstant::NEVER_OCCURRED()) {
            mStandbyStartTime = now;
        }

        Vs64 currentQueueDataSize = mStartupStandbyQueue.getQueueDataSize();
        if ((mMaxClientQueueDataSize > 0) && (currentQueueDataSize >= mMaxClientQueueDataSize)) {
            // We have hit the queue size limit. Do not post. Initiate a shutdown of this session.
            VLOGGER_NAMED_ERROR(mLoggerName, VSTRING_FORMAT("[%s] VClientSession::postOutputMessage: Reached output queue limit of " VSTRING_FORMATTER_S64 " bytes. Not posting message ID=%d. Closing socket to force shutdown of session and its i/o threads.", this->getName().chars(), mMaxClientQueueDataSize, message->getMessageID()));
            mSocket->close();
        } else if ((mStandbyTimeLimit == VDuration::ZERO()) || (now <= mStandbyStartTime + mStandbyTimeLimit)) {
            VLOGGER_NAMED_DEBUG(mLoggerName, VSTRING_FORMAT("[%s] VClientSession::postOutputMessage: Placing message ID=%d on standby queue for not-yet-started session.", this->getName().chars(), message->getMessageID()));
            mStartupStandbyQueue.postMessage(message);
        } else {
            // We have hit the standby time limit. Do not post. Initiate a shutdown of this session.
            VLOGGER_NAMED_ERROR(mLoggerName, VSTRING_FORMAT("[%s] VClientSession::postOutputMessage: Reached standby time limit of %s. Not posting message ID=%d. Closing socket to force shutdown of session and its i/o threads.", this->getName().chars(), mStandbyTimeLimit.getDurationString().chars(), message->getMessageID()));
            mSocket->close();
        }
    } else if (mOutputThread != NULL) {
        // This branch is entered only for posting to a session with an async output thread.
        // Typical for posting a message directly to 1 session or broadcasting to multiple sessions.
        // We need to post to the output thread.
        // Note that mOutputThread->postOutputMessage() stops its own thread if posting fails, triggering session end. We don't need to take action.
        mOutputThread->postOutputMessage(message);
    } else { // no output thread
        // Vault 4.0 TODO: This used to be for non-broadcast only, but I'm removing the distinction.
        // However, does this change how teardown works? Formerly the other branch (broadcast) treated
        // a null output thread as sign that we should close the socket. Hopefully that case is irrelevant
        // or will just throw an exception here if the mIOStream is dead. Here's the old comment:

        // This branch is entered for non-broadcast synchronous-session posting. Just send on the socket stream.
        // This would only be for sessions that are synchronous and do not use a separate output thread.
        // Write the message directly to our output stream and release it.
        message->send(this->getName(), mIOStream);
    }

}

void VClientSession::sendMessageToClient(VMessagePtr message, const VString& sessionLabel, VBinaryIOStream& out) {
    // No longer a need to lock mMutex, because session is ref counted and cannot disappear from under us here.
    if (mIsShuttingDown || this->isClientGoingOffline()) {
        VLOGGER_NAMED_WARN(mLoggerName, VSTRING_FORMAT("VClientSession::sendMessageToClient: NOT sending message@0x%08X to offline session [%s], presumably in process of session shutdown.", message.get(), mClientAddress.chars()));
    } else {
        VLOGGER_NAMED_LEVEL(mLoggerName, VMessage::kMessageQueueOpsLevel, VSTRING_FORMAT("[%s] VClientSession::sendMessageToClient: Sending message@0x%08X.", sessionLabel.chars(), message.get()));
        message->send(sessionLabel, out);
    }
}

VBentoNode* VClientSession::getSessionInfo() const {
    VBentoNode* result = new VBentoNode(mName);

    result->addString("name", mName);
    result->addString("type", mClientType);
    result->addString("address", mClientAddress);
    result->addString("shutting", mIsShuttingDown ? "yes" : "no");

    int standbyQueueSize = (int) mStartupStandbyQueue.getQueueSize();
    if (standbyQueueSize != 0) {
        result->addInt("standby-queue-size", (int) mStartupStandbyQueue.getQueueSize());
        result->addS64("standby-queue-data-size", mStartupStandbyQueue.getQueueDataSize());
    }

    if (mOutputThread != NULL) {
        result->addInt("output-queue-size", mOutputThread->getOutputQueueSize());
    }

    return result;
}

void VClientSession::_moveStandbyMessagesToAsyncOutputQueue() {
    // Note that we rely on the caller to lock the mMutex before calling us.
    // changeInitalizationState calls us but needs to lock a larger scope,
    // so we don't want to do the locking.
    VMessagePtr m = mStartupStandbyQueue.getNextMessage();

    while (m != nullptr) {
        VLOGGER_NAMED_TRACE(mLoggerName, VSTRING_FORMAT("[%s] VClientSession::_moveStandbyMessagesToAsyncOutputQueue: Moving message message@0x%08X from standby queue to output queue.", this->getName().chars(), m.get()));
        this->_postStandbyMessageToAsyncOutputQueue(m);
        m = mStartupStandbyQueue.getNextMessage();
    }

    mStandbyStartTime = VInstant::NEVER_OCCURRED(); // We are no longer in standby queuing mode (until next time we queue).
}

int VClientSession::_getOutputQueueSize() const {
    return (mOutputThread == NULL) ? 0 : mOutputThread->getOutputQueueSize();
}

void VClientSession::_postStandbyMessageToAsyncOutputQueue(VMessagePtr message) {
    mOutputThread->postOutputMessage(message, false /* do not respect the queue limits, just move all messages onto the queue */);
}

void VClientSession::_releaseQueuedClientMessages() {
    VMutexLocker locker(&mMutex, VSTRING_FORMAT("[%s]VClientSession::_releaseQueuedClientMessages()", this->getName().chars())); // protect the mStartupStandbyQueue during queue operations

    // Order probably does not matter, but it makes sense to pop them in the order they would have been sent.

    if (mOutputThread != NULL) {
        mOutputThread->releaseAllQueuedMessages();
    }

    mStartupStandbyQueue.releaseAllMessages();
}

// VClientSessionFactory -----------------------------------------------------------

void VClientSessionFactory::addSessionToServer(VClientSessionPtr session) {
    if (mServer != NULL) {
        mServer->addClientSession(session);
    }
}
