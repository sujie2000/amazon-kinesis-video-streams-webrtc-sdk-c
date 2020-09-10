/**
 * Kinesis Video Producer ConnectionListener
 */
#define LOG_CLASS "ConnectionListener"
#include "../Include_i.h"

STATUS createConnectionListener(PConnectionListener* ppConnectionListener)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;
    /** #memory #YC_TBD, need to improve.*/
    UINT32 allocationSize = SIZEOF(ConnectionListener) + MAX_UDP_PACKET_SIZE;
    PConnectionListener pConnectionListener = NULL;

    CHK(ppConnectionListener != NULL, STATUS_NULL_ARG);
    /**#memory*/
    pConnectionListener = (PConnectionListener) MEMCALLOC(1, allocationSize);
    CHK(pConnectionListener != NULL, STATUS_NOT_ENOUGH_MEMORY);

    CHK_STATUS(doubleListCreate(&pConnectionListener->connectionList));
    ATOMIC_STORE_BOOL(&pConnectionListener->terminate, FALSE);
    ATOMIC_STORE_BOOL(&pConnectionListener->listenerRoutineStarted, FALSE);
    /* always update list when receiveDataRoutine first start */
    ATOMIC_STORE_BOOL(&pConnectionListener->connectionListChanged, TRUE);
    pConnectionListener->receiveDataRoutine = INVALID_TID_VALUE;
    pConnectionListener->lock = MUTEX_CREATE(FALSE);
    pConnectionListener->removeConnectionComplete = CVAR_CREATE();

    // pConnectionListener->pBuffer starts at the end of ConnectionListener struct
    pConnectionListener->pBuffer = (PBYTE)(pConnectionListener + 1);
    pConnectionListener->bufferLen = MAX_UDP_PACKET_SIZE;//!< 65507

CleanUp:

    if (STATUS_FAILED(retStatus) && pConnectionListener != NULL) {
        freeConnectionListener(&pConnectionListener);
        pConnectionListener = NULL;
    }

    if (ppConnectionListener != NULL) {
        *ppConnectionListener = pConnectionListener;
    }
    LEAVES();
    return retStatus;
}

STATUS freeConnectionListener(PConnectionListener* ppConnectionListener)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;
    PConnectionListener pConnectionListener = NULL;

    CHK(ppConnectionListener != NULL, STATUS_NULL_ARG);
    CHK(*ppConnectionListener != NULL, retStatus);

    pConnectionListener = *ppConnectionListener;

    ATOMIC_STORE_BOOL(&pConnectionListener->terminate, TRUE);

    if (IS_VALID_CVAR_VALUE(pConnectionListener->removeConnectionComplete)) {
        CVAR_SIGNAL(pConnectionListener->removeConnectionComplete);
    }

    if (IS_VALID_TID_VALUE(pConnectionListener->receiveDataRoutine)) {
        THREAD_JOIN(pConnectionListener->receiveDataRoutine, NULL);
        pConnectionListener->receiveDataRoutine = INVALID_TID_VALUE;
    }

    if (pConnectionListener->connectionList != NULL) {
        CHK_LOG_ERR(doubleListClear(pConnectionListener->connectionList, FALSE));
        CHK_LOG_ERR(doubleListFree(pConnectionListener->connectionList));
    }

    if (pConnectionListener->lock != INVALID_MUTEX_VALUE) {
        MUTEX_FREE(pConnectionListener->lock);
    }

    if (IS_VALID_CVAR_VALUE(pConnectionListener->removeConnectionComplete)) {
        CVAR_FREE(pConnectionListener->removeConnectionComplete);
    }

    MEMFREE(pConnectionListener);

    *ppConnectionListener = NULL;

CleanUp:

    CHK_LOG_ERR(retStatus);
    LEAVES();
    return retStatus;
}
/**
 * @brief add one new connection and wait for the listener thread to latch it.
 * 
 * @param[in] pConnectionListener the database.
 * @param[in] pSocketConnection the new socket connection.
*/
STATUS connectionListenerAddConnection(PConnectionListener pConnectionListener, PSocketConnection pSocketConnection)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;
    BOOL locked = FALSE;

    CHK(pConnectionListener != NULL && pSocketConnection != NULL, STATUS_NULL_ARG);
    CHK(!ATOMIC_LOAD_BOOL(&pConnectionListener->terminate), retStatus);

    MUTEX_LOCK(pConnectionListener->lock);
    locked = TRUE;

    CHK_STATUS(doubleListInsertItemHead(pConnectionListener->connectionList, (UINT64) pSocketConnection));

    MUTEX_UNLOCK(pConnectionListener->lock);
    locked = FALSE;

    ATOMIC_STORE_BOOL(&pConnectionListener->connectionListChanged, TRUE);

CleanUp:

    if (locked) {
        MUTEX_UNLOCK(pConnectionListener->lock);
    }
    LEAVES();
    return retStatus;
}

STATUS connectionListenerRemoveConnection(PConnectionListener pConnectionListener, PSocketConnection pSocketConnection)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS, cvarWaitStatus = STATUS_SUCCESS;
    BOOL locked = FALSE, hasConnection = FALSE;
    PDoubleListNode pCurNode = NULL;
    PSocketConnection pCurrSocketConnection = NULL;

    CHK(pConnectionListener != NULL && pSocketConnection != NULL, STATUS_NULL_ARG);
    CHK(!ATOMIC_LOAD_BOOL(&pConnectionListener->terminate), retStatus);

    /* mark socket as closed. Will be cleaned up by connectionListenerReceiveDataRoutine */
    CHK_STATUS(socketConnectionClosed(pSocketConnection));

    ATOMIC_STORE_BOOL(&pConnectionListener->connectionListChanged, TRUE);

    MUTEX_LOCK(pConnectionListener->lock);
    locked = TRUE;

    CHK_STATUS(doubleListGetHeadNode(pConnectionListener->connectionList, &pCurNode));
    while (!hasConnection && pCurNode != NULL) {
        pCurrSocketConnection = (PSocketConnection) pCurNode->data;
        pCurNode = pCurNode->pNext;
        hasConnection = pCurrSocketConnection == pSocketConnection;
    }

    /* If connection is not found then return early */
    CHK(hasConnection, retStatus);

    /* make sure connectionListenerRemoveConnection return after connectionListenerReceiveDataRoutine has picked up
     * the change. */
    while (ATOMIC_LOAD_BOOL(&pConnectionListener->listenerRoutineStarted) && !ATOMIC_LOAD_BOOL(&pConnectionListener->terminate) &&
           ATOMIC_LOAD_BOOL(&pConnectionListener->connectionListChanged) && STATUS_SUCCEEDED(cvarWaitStatus)) {
        cvarWaitStatus =
            CVAR_WAIT(pConnectionListener->removeConnectionComplete, pConnectionListener->lock, CONNECTION_AWAIT_CONNECTION_REMOVAL_TIMEOUT);
        /* CVAR_WAIT should never time out */
        if (STATUS_FAILED(cvarWaitStatus)) {
            DLOGW("CVAR_WAIT() failed with 0x%08x", cvarWaitStatus);
        }
    }

CleanUp:

    if (locked) {
        MUTEX_UNLOCK(pConnectionListener->lock);
    }
    LEAVES();
    return retStatus;
}

STATUS connectionListenerRemoveAllConnection(PConnectionListener pConnectionListener)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS, cvarWaitStatus = STATUS_SUCCESS;
    BOOL locked = FALSE;
    PDoubleListNode pCurNode = NULL;
    PSocketConnection pSocketConnection = NULL;

    CHK(pConnectionListener != NULL, STATUS_NULL_ARG);
    CHK(!ATOMIC_LOAD_BOOL(&pConnectionListener->terminate), retStatus);

    MUTEX_LOCK(pConnectionListener->lock);
    locked = TRUE;

    // mark all socket as closed. Will be cleaned up by connectionListenerReceiveDataRoutine
    CHK_STATUS(doubleListGetHeadNode(pConnectionListener->connectionList, &pCurNode));
    while (pCurNode != NULL) {
        pSocketConnection = (PSocketConnection) pCurNode->data;
        pCurNode = pCurNode->pNext;
        CHK_STATUS(socketConnectionClosed(pSocketConnection));
    }

    ATOMIC_STORE_BOOL(&pConnectionListener->connectionListChanged, TRUE);

    /* make sure connectionListenerRemoveAllConnection return after connectionListenerReceiveDataRoutine has picked up
     * the change. */
    /** #YC_TBD, need to review this part since it is not efficient. */
    while (ATOMIC_LOAD_BOOL(&pConnectionListener->listenerRoutineStarted) && 
           !ATOMIC_LOAD_BOOL(&pConnectionListener->terminate) &&
           ATOMIC_LOAD_BOOL(&pConnectionListener->connectionListChanged) && 
           STATUS_SUCCEEDED(cvarWaitStatus)) {

        cvarWaitStatus =
            CVAR_WAIT(pConnectionListener->removeConnectionComplete, pConnectionListener->lock, CONNECTION_AWAIT_CONNECTION_REMOVAL_TIMEOUT);
        /* CVAR_WAIT should never time out */
        if (STATUS_FAILED(cvarWaitStatus)) {
            DLOGW("CVAR_WAIT() failed with 0x%08x", cvarWaitStatus);
        }
    }

CleanUp:

    if (locked) {
        MUTEX_UNLOCK(pConnectionListener->lock);
    }
    LEAVES();
    return retStatus;
}
/**
 * Spin off a listener thread that listen for incoming traffic for all PSocketConnection stored in connectionList.
 * Whenever a PSocketConnection receives data, invoke ConnectionDataAvailableFunc passed in.
 *
 * @param - PConnectionListener      - IN - the ConnectionListener struct to use
 *
 * @return - STATUS status of execution
 */
STATUS connectionListenerStart(PConnectionListener pConnectionListener)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;
    ATOMIC_BOOL listenerRoutineStarted = FALSE;

    CHK(pConnectionListener != NULL, STATUS_NULL_ARG);
    CHK(!ATOMIC_LOAD_BOOL(&pConnectionListener->terminate), retStatus);
    listenerRoutineStarted = ATOMIC_EXCHANGE_BOOL(&pConnectionListener->listenerRoutineStarted, TRUE);
    CHK(!listenerRoutineStarted, retStatus);
    /** #thread. */
    /** #task. */
    CHK_STATUS(THREAD_CREATE(&pConnectionListener->receiveDataRoutine, connectionListenerReceiveDataRoutine, (PVOID) pConnectionListener));

CleanUp:
    LEAVES();
    return retStatus;
}
/**
 * @brief the task handler. the initialization code will create one listener thread to handler these connections.
 * 
 * @param 
*/
PVOID connectionListenerReceiveDataRoutine(PVOID arg)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;
    PConnectionListener pConnectionListener = (PConnectionListener) arg;
    PDoubleListNode pCurNode = NULL, pNodeToDelete = NULL;
    PSocketConnection pSocketConnection;
    BOOL locked = FALSE, iterate = TRUE, updateSocketList = FALSE, connectionListChanged = FALSE;
    /** #memory #YC_TBD, need to improve.*/
    PSocketConnection socketList[CONNECTION_LISTENER_DEFAULT_MAX_LISTENING_CONNECTION];
    UINT32 socketCount = 0, i;

    INT32 nfds = 0;
    fd_set rfds;
    struct timeval tv;
    INT32 retval;
    INT64 readLen;
    // the source address is put here. sockaddr_storage can hold either sockaddr_in or sockaddr_in6
    /** #memory. */
    struct sockaddr_storage srcAddrBuff;
    socklen_t srcAddrBuffLen = SIZEOF(srcAddrBuff);
    struct sockaddr_in* pIpv4Addr;
    struct sockaddr_in6* pIpv6Addr;
    KvsIpAddress srcAddr;
    PKvsIpAddress pSrcAddr = NULL;

    CHK(pConnectionListener != NULL, STATUS_NULL_ARG);

    /* Ensure that memory sanitizers consider
     * rfds initialized even if FD_ZERO is
     * implemented in assembly. */
    MEMSET(&rfds, 0x00, SIZEOF(fd_set));

    srcAddr.isPointToPoint = FALSE;

    while (!ATOMIC_LOAD_BOOL(&pConnectionListener->terminate)) {
        FD_ZERO(&rfds);
        nfds = 0;

        // update connection list.
        /**
         * refresh the connectionlist if connection list is changed or socket list is updated.
         * 
        */
        connectionListChanged = ATOMIC_LOAD_BOOL(&pConnectionListener->connectionListChanged);
        
        if (connectionListChanged || updateSocketList) {
            MUTEX_LOCK(pConnectionListener->lock);
            locked = TRUE;

            socketCount = 0;
            /** delete the inactive connections from the connection list. */
            CHK_STATUS(doubleListGetHeadNode(pConnectionListener->connectionList, &pCurNode));
            while (pCurNode != NULL) {
                pSocketConnection = (PSocketConnection) pCurNode->data;

                if (ATOMIC_LOAD_BOOL(&pSocketConnection->connectionClosed)) {
                    pNodeToDelete = pCurNode;
                    pCurNode = pCurNode->pNext;

                    CHK_STATUS(doubleListDeleteNode(pConnectionListener->connectionList, pNodeToDelete));
                } else {
                    pCurNode = pCurNode->pNext;
                    if (socketCount < ARRAY_SIZE(socketList)) {
                        socketList[socketCount] = pSocketConnection;
                        socketCount++;
                    } else {
                        DLOGW("Max socket list size of %u exceeded. Will not receive data from socket %d", ARRAY_SIZE(socketList),
                              pSocketConnection->localSocket);
                        break;
                    }
                }
            }

            MUTEX_UNLOCK(pConnectionListener->lock);
            locked = FALSE;

            updateSocketList = FALSE;

            if (connectionListChanged) {
                ATOMIC_STORE_BOOL(&pConnectionListener->connectionListChanged, FALSE);
                CVAR_BROADCAST(pConnectionListener->removeConnectionComplete);
            }
        }
        /**
         * 
         * 
         * 
        */
        for (i = 0; i < socketCount; ++i) {
            pSocketConnection = socketList[i];
            /** remove the closed sockets. */
            if (socketConnectionIsClosed(pSocketConnection)) {
                //DLOGD("isclosed");
                updateSocketList = TRUE;
            } else {
                //DLOGD("pSocketConnection->localSocket:%x", pSocketConnection->localSocket);
                FD_SET(pSocketConnection->localSocket, &rfds);
                nfds = MAX(nfds, pSocketConnection->localSocket);
            }
        }

        nfds++;

        // timeout select every SOCKET_WAIT_FOR_DATA_TIMEOUT_SECONDS seconds and check if terminate
        // on linux tv need to be reinitialized after select is done.
        /**
         * poll the socket about 1 seconds.
        */
        tv.tv_sec = SOCKET_WAIT_FOR_DATA_TIMEOUT_SECONDS;
        tv.tv_usec = 0;

        // blocking call
        /** polling. #YC_TBD, */
        retval = select(nfds, &rfds, NULL, NULL, &tv);
        //DLOGD("up");
        if (retval == -1) {
            DLOGE("select() failed with errno %s", strerror(errno));
            continue;
        } else if (retval == 0) {
            continue;
        }

        for (i = 0; i < socketCount; ++i) {
            pSocketConnection = socketList[i];

            /** check the status of socket connections. */
            if (socketConnectionIsClosed(pSocketConnection)) {
                /* update the connection list to remove the closed sockets */
                updateSocketList = TRUE;
            } 
            else if (FD_ISSET(pSocketConnection->localSocket, &rfds)) 
            {
                iterate = TRUE;
                while (iterate) {
                    /** #socket. */
                    readLen = recvfrom(pSocketConnection->localSocket,
                                       pConnectionListener->pBuffer,
                                       pConnectionListener->bufferLen,
                                       0,
                                       (struct sockaddr*) &srcAddrBuff,
                                       &srcAddrBuffLen);
                    /** #YC_TBD, need to review. */
                    //DLOGD("readLen:%d", readLen);
                    if (readLen < 0) {
                        switch (errno) {
                            case EWOULDBLOCK:
                                break;
                            default:
                                /* on any other error, close connection */
                                CHK_STATUS(socketConnectionClosed(pSocketConnection));
                                //DLOGD("recvfrom() failed with errno %s for socket %d", strerror(errno), pSocketConnection->localSocket);
                                break;
                        }

                        iterate = FALSE;
                    }
                    else if (readLen == 0)
                    {
                        CHK_STATUS(socketConnectionClosed(pSocketConnection));
                        iterate = FALSE;
                    }
                    else if (/* readLen > 0 */
                               ATOMIC_LOAD_BOOL(&pSocketConnection->receiveData) && 
                               pSocketConnection->dataAvailableCallbackFn != NULL &&
                               /* data could be encrypted so they need to be decrypted through socketConnectionReadData
                                * and get the decrypted data length. */
                               STATUS_SUCCEEDED(socketConnectionReadData(pSocketConnection, 
                                                                         pConnectionListener->pBuffer,
                                                                         pConnectionListener->bufferLen, 
                                                                         (PUINT32) &readLen))) {
                        /** #UDP.*/
                        if (pSocketConnection->protocol == KVS_SOCKET_PROTOCOL_UDP) {
                            if (srcAddrBuff.ss_family == AF_INET) {
                                srcAddr.family = KVS_IP_FAMILY_TYPE_IPV4;
                                pIpv4Addr = (struct sockaddr_in*) &srcAddrBuff;
                                MEMCPY(srcAddr.address, (PBYTE) &pIpv4Addr->sin_addr, IPV4_ADDRESS_LENGTH);
                                srcAddr.port = pIpv4Addr->sin_port;
                            } else if (srcAddrBuff.ss_family == AF_INET6) {
                                srcAddr.family = KVS_IP_FAMILY_TYPE_IPV6;
                                pIpv6Addr = (struct sockaddr_in6*) &srcAddrBuff;
                                MEMCPY(srcAddr.address, (PBYTE) &pIpv6Addr->sin6_addr, IPV6_ADDRESS_LENGTH);
                                srcAddr.port = pIpv6Addr->sin6_port;
                            }
                            pSrcAddr = &srcAddr;
                        } 
                        /** #TCP. */
                        else {
                            // srcAddr is ignored in TCP callback handlers
                            pSrcAddr = NULL;
                        }

                        // readLen may be 0 if SSL does not emit any application data.
                        // in that case, no need to call dataAvailable callback
                        if (readLen > 0) {
                            /** #YC_TBD, need to confirm the length is bigger than the packet size. */
                            pSocketConnection->dataAvailableCallbackFn(pSocketConnection->dataAvailableCallbackCustomData, pSocketConnection,
                                                                       pConnectionListener->pBuffer, (UINT32) readLen, pSrcAddr,
                                                                       NULL); // no dest information available right now.
                        }
                    }

                    // reset srcAddrBuffLen to actual size
                    srcAddrBuffLen = SIZEOF(srcAddrBuff);
                }
            }
        }
    }

CleanUp:

    if (locked) {
        MUTEX_UNLOCK(pConnectionListener->lock);
    }

    CHK_LOG_ERR(retStatus);

    ATOMIC_STORE_BOOL(&pConnectionListener->listenerRoutineStarted, FALSE);
    LEAVES();
    return (PVOID)(ULONG_PTR) retStatus;
}
