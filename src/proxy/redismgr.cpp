#include "redismgr.h"
#include "hash.h"
#include "twemroute.h"
#include "codisroute.h"
#include "gr_clusterroute.h"

GR_RedisMgr *GR_RedisMgr::m_pInstance = new GR_RedisMgr();

GR_RedisMgr *GR_RedisMgr::Instance()
{
    return m_pInstance;
}

GR_RedisMgr::GR_RedisMgr()
{
}

GR_RedisMgr::~GR_RedisMgr()
{
    if (this->m_pSentinelMgr != nullptr)
    {
        delete this->m_pSentinelMgr;
    }
}

int GR_RedisMgr::Init(GR_Config *pConfig)
{
    this->m_pConfig = pConfig;
    // 初始化路由信息
    switch (pConfig->m_iRouteMode)
    {
        case PROXY_ROUTE_TWEM:
        {
            GR_TwemRoute *pRoute = new GR_TwemRoute();
            this->m_pRoute = pRoute;
            if (GR_OK != this->m_pRoute->Init(pConfig))
            {
                GR_LOGE("init twem route failed");
                return GR_ERROR;
            }
            // 将监听信息通过配置传递出去
            if (!this->m_pRoute->GetListenInfo(pConfig->m_strIP, pConfig->m_usPort, pConfig->m_iTcpBack))
            {
                GR_LOGE("get listen address failed");
                return GR_ERROR;
            }

            // 根据配置走不同的初始化流程
            if (!this->m_pRoute->m_bSentinel)
            {
                return this->ConnectToRedis();
            }
            else // 先和sentinel建立连接
            {
                this->m_pSentinelMgr = new GR_SentinelMgr(pRoute);
                return this->m_pSentinelMgr->ConnectToSentinel();
            }
            break;
        }
        case PROXY_ROUTE_CODIS:
        {
            GR_LOGE("not support work mode:%d", pConfig->m_iRouteMode);
            return GR_ERROR;
        }
        case PROXY_ROUTE_CLUSTER:
        {
            GR_ClusterRoute *pRoute = new GR_ClusterRoute();
            this->m_pRoute = pRoute;
            if (GR_OK != this->m_pRoute->Init(pConfig))
            {
                GR_LOGE("cluster route init failed.");
                return GR_ERROR;
            }
            break;
        }
        default:
        {
            GR_LOGE("not support work mode:%d", pConfig->m_iRouteMode);
            return GR_ERROR;
        }
    }
}

int GR_RedisMgr::ConnectToRedis()
{
    int iRet;
    GR_RedisServer *pServer;
    for (int i=0; i<this->m_pRoute->m_iSrvNum; i++)
    {
        pServer = this->m_pRoute->m_vServers[i];
        iRet = pServer->Connect();
        if (iRet != GR_OK)
        {
            GR_LOGE("connect to redis failed:%s", pServer->strInfo.c_str());
            return GR_ERROR;
        }
    }
    return GR_OK;
}

int GR_RedisMgr::ReplicateMsgToRedis(GR_ReplicaEvent *pEvent, GR_MsgIdenty *pIdenty)
{
    ASSERT(pEvent->m_ReadMsg.m_Info.iKeyLen > 0);
    int iRet = GR_OK;
    pEvent->m_ReadCache.m_pData->m_sUsedSize = pEvent->m_ReadMsg.m_Info.iLen;
    m_pTempRedis = this->m_pRoute->Route(pIdenty, pEvent->m_ReadCache.m_pData, pEvent->m_ReadMsg, iRet);
    if (iRet != GR_OK)
    {
        return iRet;
    }
    if (m_pTempRedis == nullptr || m_pTempRedis->m_iFD <= 0)
    {
        // TODO 先放缓冲池中，等redis连接上之后发送给redis
        GR_LOGE("transfer message get redis failed");
        return REDIS_RSP_DISCONNECT;
    }
    return m_pTempRedis->SendMsg(pEvent->m_ReadCache.m_pData, pIdenty);
}

int GR_RedisMgr::TransferMsgToRedis(GR_AccessEvent *pEvent, GR_MsgIdenty *pIdenty)
{
    ASSERT(pEvent->m_ReadMsg.m_Info.iKeyLen > 0);
    int iRet = GR_OK;
    pEvent->m_ReadCache.m_pData->m_sUsedSize = pEvent->m_ReadMsg.m_Info.iLen;
    m_pTempRedis = this->m_pRoute->Route(pIdenty, pEvent->m_ReadCache.m_pData, pEvent->m_ReadMsg, iRet);
    if (iRet != GR_OK)
    {
        ASSERT(false);
        return iRet;
    }
    if (m_pTempRedis == nullptr || m_pTempRedis->m_iFD <= 0)
    {
        // TODO 先放缓冲池中，等redis连接上之后发送给redis
        GR_LOGE("transfer message get redis failed");
        //ASSERT(false);
        return REDIS_RSP_DISCONNECT;
    }
    return m_pTempRedis->SendMsg(pEvent->m_ReadCache.m_pData, pIdenty);
}

int GR_RedisMgr::RedisConnected(uint16 uiPort, char *szAddr)
{
    // TODO 全部连接上redis之后再起外部监听端口
    this->m_iConnectedNum+=1;
    return GR_OK;
}

int GR_RedisMgr::SentinelConnected(GR_SentinelEvent *pEvent)
{
    return this->m_pSentinelMgr->SentinelConnected(pEvent);
}

int GR_RedisMgr::LoopCheck()
{
    return GR_OK;
    ASSERT(this->m_pRoute!=nullptr);
    try
    {
        GR_RedisServer  *pServer = nullptr;
        GR_RedisEvent   *pEvent = nullptr;
        uint64 ulNow = CURRENT_MS();
        for (int i=0; i<this->m_pRoute->m_iSrvNum; i++)
        {
            pServer = this->m_pRoute->m_vServers[i];
            if (pServer == nullptr || pServer->pEvent == nullptr)
            {
                continue;
            }
            pEvent = pServer->pEvent;
            if (pEvent->m_Status != GR_CONNECT_CONNECTED || pEvent->m_ulFirstMsgMS == 0)
            {
                continue;
            }
            if (ulNow - pEvent->m_ulFirstMsgMS > pEvent->m_iRedisRspTT)
            {
                // 由于删除之后最后一个移动到当前位置，所以就少遍历了，不过没关系，下次遍历就遍历到了
                pEvent->Close();
                GR_LOGE("redis response tt %ld, %ld, %d", ulNow,  pEvent->m_ulFirstMsgMS, pEvent->m_iRedisRspTT);
               continue;
            }
        }

    }
    catch(exception &e)
    {
        GR_LOGE("redis loop check got exception:%s", e.what());
        return GR_ERROR;
    }

    return GR_OK;
}

