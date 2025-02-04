#include <ydb/core/keyvalue/keyvalue_events.h>
#include <ydb/core/persqueue/events/internal.h>
#include <ydb/core/persqueue/partition.h>
#include <ydb/core/persqueue/ut/common/pq_ut_common.h>
#include <ydb/core/protos/counters_keyvalue.pb.h>
#include <ydb/core/protos/pqconfig.pb.h>
#include <ydb/core/tablet/tablet_counters_protobuf.h>
#include <ydb/library/persqueue/topic_parser/topic_parser.h>
#include <ydb/public/api/protos/draft/persqueue_error_codes.pb.h>
#include <ydb/public/lib/base/msgbus_status.h>

#include <library/cpp/actors/core/actorid.h>
#include <library/cpp/actors/core/event.h>
#include <library/cpp/testing/unittest/registar.h>

#include <util/generic/hash.h>
#include <util/generic/maybe.h>
#include <util/generic/ptr.h>
#include <util/generic/string.h>
#include <util/system/types.h>

namespace NKikimr::NPQ {

Y_UNIT_TEST_SUITE(TUserActionProcessorTests) {

namespace NHelpers {

struct TCreatePartitionParams {
    ui32 Partition = 1;
    ui64 Begin = 0;
    ui64 End = 0;
    TMaybe<ui64> PlanStep;
    TMaybe<ui64> TxId;
    TVector<TTransaction> Transactions;
};

struct TCreateConsumerParams {
    TString Consumer;
    ui64 Offset = 0;
    ui32 Generation = 0;
    ui32 Step = 0;
    TString Session;
    ui64 OffsetRewindSum = 0;
    ui64 ReadRuleGeneration = 0;
};

}

class TUserActionProcessorFixture : public NUnitTest::TBaseFixture {
protected:
    struct TUserInfoMatcher {
        TMaybe<TString> Session;
        TMaybe<ui64> Offset;
        TMaybe<ui32> Generation;
        TMaybe<ui32> Step;
    };

    struct TCmdWriteMatcher {
        TMaybe<size_t> Count;
        TMaybe<ui64> PlanStep;
        TMaybe<ui64> TxId;
        THashMap<size_t, TUserInfoMatcher> UserInfos;
    };

    struct TProxyResponseMatcher {
        TMaybe<ui64> Cookie;
        TMaybe<NMsgBusProxy::EResponseStatus> Status;
        TMaybe<NPersQueue::NErrorCode::EErrorCode> ErrorCode;
        TMaybe<ui64> Offset;
    };

    struct TErrorMatcher {
        TMaybe<ui64> Cookie;
        TMaybe<NPersQueue::NErrorCode::EErrorCode> ErrorCode;
        TMaybe<TString> Error;
    };

    struct TProposeTransactionResponseMatcher {
        TMaybe<ui64> TxId;
        TMaybe<NKikimrPQ::TEvProposeTransactionResult::EStatus> Status;
    };

    struct TCalcPredicateMatcher {
        TMaybe<ui64> Step;
        TMaybe<ui64> TxId;
        TMaybe<ui32> Partition;
        TMaybe<bool> Predicate;
    };

    struct TCommitTxDoneMatcher {
        TMaybe<ui64> Step;
        TMaybe<ui64> TxId;
        TMaybe<ui32> Partition;
    };

    using TCreatePartitionParams = NHelpers::TCreatePartitionParams;
    using TCreateConsumerParams = NHelpers::TCreateConsumerParams;

    void SetUp(NUnitTest::TTestContext&) override;
    void TearDown(NUnitTest::TTestContext&) override;

    void CreatePartitionActor(ui32 partition,
                              const TVector<TCreateConsumerParams>& consumers,
                              bool newPartition,
                              TVector<TTransaction> txs);
    void CreatePartition(const TCreatePartitionParams& params = {},
                         const TVector<TCreateConsumerParams>& consumers = {});

    void CreateSession(const TString& clientId,
                       const TString& sessionId,
                       ui32 generation = 1, ui32 step = 1,
                       ui64 cookie = 1);
    void SetOffset(const TString& clientId,
                   const TString& sessionId,
                   ui64 offset,
                   TMaybe<ui64> expected = Nothing(),
                   ui64 cookie = 1);

    void SendCreateSession(ui64 cookie,
                           const TString& clientId,
                           const TString& sessionId,
                           ui32 generation,
                           ui32 step);
    void SendSetOffset(ui64 cookie,
                       const TString& clientId,
                       ui64 offset,
                       const TString& sessionId);
    void SendGetOffset(ui64 cookie,
                       const TString& clientId);
    void WaitCmdWrite(const TCmdWriteMatcher& matcher = {});
    void SendCmdWriteResponse(NMsgBusProxy::EResponseStatus status);
    void WaitProxyResponse(const TProxyResponseMatcher &matcher = {});
    void WaitErrorResponse(const TErrorMatcher& matcher = {});

    void WaitDiskStatusRequest();
    void SendDiskStatusResponse();
    void WaitMetaReadRequest();
    void SendMetaReadResponse(TMaybe<ui64> step, TMaybe<ui64> txId);
    void WaitInfoRangeRequest();
    void SendInfoRangeResponse(ui32 partition,
                               const TVector<TCreateConsumerParams>& consumers);
    void WaitDataRangeRequest();
    void SendDataRangeResponse(ui64 begin, ui64 end);
    void WaitDataReadRequest();
    void SendDataReadResponse();

    void SendProposeTransactionRequest(ui32 partition,
                                       ui64 begin, ui64 end,
                                       const TString& client,
                                       const TString& topic,
                                       bool immediate,
                                       ui64 txId);
    void WaitProposeTransactionResponse(const TProposeTransactionResponseMatcher& matcher = {});

    void SendCalcPredicate(ui64 step,
                           ui64 txId,
                           const TString& consumer,
                           ui64 begin,
                           ui64 end);
    void WaitCalcPredicateResult(const TCalcPredicateMatcher& matcher = {});

    void SendCommitTx(ui64 step, ui64 txId);
    void SendRollbackTx(ui64 step, ui64 txId);
    void WaitCommitTxDone(const TCommitTxDoneMatcher& matcher = {});

    TTransaction MakeTransaction(ui64 step, ui64 txId,
                                 TString consumer,
                                 ui64 begin, ui64 end,
                                 TMaybe<bool> predicate = Nothing());

private:
    TMaybe<TTestContext> Ctx;
    TMaybe<TFinalizer> Finalizer;

    TActorId ActorId;
};

void TUserActionProcessorFixture::SetUp(NUnitTest::TTestContext&)
{
    Ctx.ConstructInPlace();
    Finalizer.ConstructInPlace(*Ctx);

    Ctx->Prepare();
    Ctx->Runtime->SetScheduledLimit(5'000);
}

void TUserActionProcessorFixture::TearDown(NUnitTest::TTestContext&)
{
}

void TUserActionProcessorFixture::CreatePartitionActor(ui32 id,
                                                       const TVector<TCreateConsumerParams>& consumers,
                                                       bool newPartition,
                                                       TVector<TTransaction> txs)
{
    using TKeyValueCounters = TProtobufTabletCounters<
        NKeyValue::ESimpleCounters_descriptor,
        NKeyValue::ECumulativeCounters_descriptor,
        NKeyValue::EPercentileCounters_descriptor,
        NKeyValue::ETxTypes_descriptor
    >;
    using TPersQueueCounters = TAppProtobufTabletCounters<
        NPQ::ESimpleCounters_descriptor,
        NPQ::ECumulativeCounters_descriptor,
        NPQ::EPercentileCounters_descriptor
    >;
    using TCounters = TProtobufTabletCountersPair<
        TKeyValueCounters,
        TPersQueueCounters
    >;

    TAutoPtr<TCounters> counters(new TCounters());
    TAutoPtr<TTabletCountersBase> tabletCounters = counters->GetSecondTabletCounters().Release();

    NPersQueue::TTopicNamesConverterFactory factory(true, "/Root/PQ", "dc1");
    NPersQueue::TTopicConverterPtr topicConverter;
    NKikimrPQ::TPQTabletConfig config;

    for (auto& c : consumers) {
        config.AddReadRules(c.Consumer);
    }

    config.SetTopicName("rt3.dc1--account--topic");
    config.SetTopicPath("/Root/PQ/rt3.dc1--account--topic");
    config.SetFederationAccount("account");
    config.SetLocalDC(true);
    config.SetYdbDatabasePath("");

    topicConverter = factory.MakeTopicConverter(config);

    auto actor = new NPQ::TPartition(Ctx->TabletId,
                                     id,
                                     Ctx->Edge,
                                     Ctx->Edge,
                                     topicConverter,
                                     true,
                                     "dcId",
                                     false,
                                     config,
                                     *tabletCounters,
                                     newPartition,
                                     std::move(txs));
    ActorId = Ctx->Runtime->Register(actor);
}

void TUserActionProcessorFixture::CreatePartition(const TCreatePartitionParams& params,
                                                  const TVector<TCreateConsumerParams>& consumers)
{
    if ((params.Begin == 0) && (params.End == 0)) {
        CreatePartitionActor(params.Partition, consumers, true, {});
    } else {
        CreatePartitionActor(params.Partition, consumers, false, params.Transactions);

        WaitDiskStatusRequest();
        SendDiskStatusResponse();

        WaitMetaReadRequest();
        SendMetaReadResponse(params.PlanStep, params.TxId);

        WaitInfoRangeRequest();
        SendInfoRangeResponse(params.Partition, consumers);

        WaitDataRangeRequest();
        SendDataRangeResponse(params.Begin, params.End);
    }
}

void TUserActionProcessorFixture::CreateSession(const TString& clientId,
                                                const TString& sessionId,
                                                ui32 generation, ui32 step,
                                                ui64 cookie)
{
    SendCreateSession(cookie,clientId,sessionId, generation, step);
    WaitCmdWrite({.Count=2, .UserInfos={{0, {.Session = sessionId, .Offset = 0}}}});
    SendCmdWriteResponse(NMsgBusProxy::MSTATUS_OK);
    WaitProxyResponse({.Cookie = cookie});
}

void TUserActionProcessorFixture::SetOffset(const TString& clientId,
                                            const TString& sessionId,
                                            ui64 offset,
                                            TMaybe<ui64> expected,
                                            ui64 cookie)
{
    SendSetOffset(cookie, clientId, offset, sessionId);
    WaitCmdWrite({.Count=2, .UserInfos={{0, {.Session = sessionId, .Offset = (expected ? *expected : offset)}}}});
    SendCmdWriteResponse(NMsgBusProxy::MSTATUS_OK);
    WaitProxyResponse({.Cookie = cookie});
}

void TUserActionProcessorFixture::SendCreateSession(ui64 cookie,
                                                    const TString& clientId,
                                                    const TString& sessionId,
                                                    ui32 generation,
                                                    ui32 step)
{
    auto event = MakeHolder<TEvPQ::TEvSetClientInfo>(cookie,
                                                     clientId,
                                                     0,
                                                     sessionId,
                                                     generation,
                                                     step,
                                                     TEvPQ::TEvSetClientInfo::ESCI_CREATE_SESSION);
    Ctx->Runtime->SingleSys()->Send(new IEventHandle(ActorId, Ctx->Edge, event.Release()));
}

void TUserActionProcessorFixture::SendSetOffset(ui64 cookie,
                                                const TString& clientId,
                                                ui64 offset,
                                                const TString& sessionId)
{
    auto event = MakeHolder<TEvPQ::TEvSetClientInfo>(cookie,
                                                     clientId,
                                                     offset,
                                                     sessionId,
                                                     0,
                                                     0);
    Ctx->Runtime->SingleSys()->Send(new IEventHandle(ActorId, Ctx->Edge, event.Release()));
}

void TUserActionProcessorFixture::SendGetOffset(ui64 cookie,
                                                const TString& clientId)
{
    auto event = MakeHolder<TEvPQ::TEvGetClientOffset>(cookie,
                                                       clientId);
    Ctx->Runtime->SingleSys()->Send(new IEventHandle(ActorId, Ctx->Edge, event.Release()));
}

void TUserActionProcessorFixture::WaitCmdWrite(const TCmdWriteMatcher& matcher)
{
    auto event = Ctx->Runtime->GrabEdgeEvent<TEvKeyValue::TEvRequest>();
    UNIT_ASSERT(event != nullptr);

    UNIT_ASSERT_VALUES_EQUAL(event->Record.GetCookie(), 1);             // SET_OFFSET_COOKIE

    if (matcher.Count.Defined()) {
        UNIT_ASSERT_VALUES_EQUAL(*matcher.Count, event->Record.CmdWriteSize());
    }
    if (matcher.PlanStep.Defined()) {
        NKikimrPQ::TPartitionTxMeta meta;
        UNIT_ASSERT(meta.ParseFromString(event->Record.GetCmdWrite(0).GetValue()));

        UNIT_ASSERT_VALUES_EQUAL(*matcher.PlanStep, meta.GetPlanStep());
    }
    if (matcher.TxId.Defined()) {
        NKikimrPQ::TPartitionTxMeta meta;
        UNIT_ASSERT(meta.ParseFromString(event->Record.GetCmdWrite(0).GetValue()));

        UNIT_ASSERT_VALUES_EQUAL(*matcher.TxId, meta.GetTxId());
    }
    for (auto& [index, userInfo] : matcher.UserInfos) {
        if (matcher.Count.Defined()) {
            UNIT_ASSERT(index < *matcher.Count);
        }

        NKikimrPQ::TUserInfo ud;
        UNIT_ASSERT(ud.ParseFromString(event->Record.GetCmdWrite(index).GetValue()));

        if (userInfo.Session) {
            UNIT_ASSERT(ud.HasSession());
            UNIT_ASSERT_VALUES_EQUAL(*userInfo.Session, ud.GetSession());
        }
        if (userInfo.Generation) {
            UNIT_ASSERT(ud.HasGeneration());
            UNIT_ASSERT_VALUES_EQUAL(*userInfo.Generation, ud.GetGeneration());
        }
        if (userInfo.Step) {
            UNIT_ASSERT(ud.HasStep());
            UNIT_ASSERT_VALUES_EQUAL(*userInfo.Step, ud.GetStep());
        }
        if (userInfo.Offset) {
            UNIT_ASSERT(ud.HasOffset());
            UNIT_ASSERT_VALUES_EQUAL(*userInfo.Offset, ud.GetOffset());
        }
    }
}

void TUserActionProcessorFixture::SendCmdWriteResponse(NMsgBusProxy::EResponseStatus status)
{
    auto event = MakeHolder<TEvKeyValue::TEvResponse>();
    event->Record.SetStatus(status);
    event->Record.SetCookie(1); // SET_OFFSET_COOKIE

    Ctx->Runtime->SingleSys()->Send(new IEventHandle(ActorId, Ctx->Edge, event.Release()));
}

void TUserActionProcessorFixture::WaitProxyResponse(const TProxyResponseMatcher& matcher)
{
    auto event = Ctx->Runtime->GrabEdgeEvent<TEvPQ::TEvProxyResponse>();
    UNIT_ASSERT(event != nullptr);

    if (matcher.Cookie) {
        UNIT_ASSERT_VALUES_EQUAL(*matcher.Cookie, event->Cookie);
    }

    if (matcher.Status) {
        UNIT_ASSERT(event->Response.HasStatus());
        UNIT_ASSERT(*matcher.Status == event->Response.GetStatus());
    }

    if (matcher.ErrorCode) {
        UNIT_ASSERT(event->Response.HasErrorCode());
        UNIT_ASSERT(*matcher.ErrorCode == event->Response.GetErrorCode());
    }

    if (matcher.Offset) {
        UNIT_ASSERT(event->Response.HasPartitionResponse());
        UNIT_ASSERT(event->Response.GetPartitionResponse().HasCmdGetClientOffsetResult());
        UNIT_ASSERT_VALUES_EQUAL(*matcher.Offset, event->Response.GetPartitionResponse().GetCmdGetClientOffsetResult().GetOffset());
    }
}

void TUserActionProcessorFixture::WaitErrorResponse(const TErrorMatcher& matcher)
{
    auto event = Ctx->Runtime->GrabEdgeEvent<TEvPQ::TEvError>();
    UNIT_ASSERT(event != nullptr);

    if (matcher.Cookie) {
        UNIT_ASSERT_VALUES_EQUAL(*matcher.Cookie, event->Cookie);
    }

    if (matcher.ErrorCode) {
        UNIT_ASSERT(*matcher.ErrorCode == event->ErrorCode);
    }

    if (matcher.Error) {
        UNIT_ASSERT_VALUES_EQUAL(*matcher.Error, event->Error);
    }
}

void TUserActionProcessorFixture::WaitDiskStatusRequest()
{
    auto event = Ctx->Runtime->GrabEdgeEvent<TEvKeyValue::TEvRequest>();
    UNIT_ASSERT(event != nullptr);

    UNIT_ASSERT(event->Record.CmdGetStatusSize() > 0);
}

void TUserActionProcessorFixture::SendDiskStatusResponse()
{
    auto event = MakeHolder<TEvKeyValue::TEvResponse>();
    event->Record.SetStatus(NMsgBusProxy::MSTATUS_OK);

    auto result = event->Record.AddGetStatusResult();
    result->SetStatus(NKikimrProto::OK);
    result->SetStatusFlags(NKikimrBlobStorage::StatusIsValid);

    Ctx->Runtime->SingleSys()->Send(new IEventHandle(ActorId, Ctx->Edge, event.Release()));
}

void TUserActionProcessorFixture::WaitMetaReadRequest()
{
    auto event = Ctx->Runtime->GrabEdgeEvent<TEvKeyValue::TEvRequest>();
    UNIT_ASSERT(event != nullptr);

    UNIT_ASSERT_VALUES_EQUAL(event->Record.CmdReadSize(), 2);
}

void TUserActionProcessorFixture::SendMetaReadResponse(TMaybe<ui64> step, TMaybe<ui64> txId)
{
    auto event = MakeHolder<TEvKeyValue::TEvResponse>();
    event->Record.SetStatus(NMsgBusProxy::MSTATUS_OK);

    //
    // NKikimrPQ::TPartitionMeta
    //
    auto read = event->Record.AddReadResult();
    read->SetStatus(NKikimrProto::NODATA);

    //
    // NKikimrPQ::TPartitionTxMeta
    //
    read = event->Record.AddReadResult();
    if (step.Defined() || txId.Defined()) {
        NKikimrPQ::TPartitionTxMeta meta;

        if (step.Defined()) {
            meta.SetPlanStep(*step);
        }
        if (txId.Defined()) {
            meta.SetTxId(*txId);
        }

        TString out;
        Y_PROTOBUF_SUPPRESS_NODISCARD meta.SerializeToString(&out);

        read->SetStatus(NKikimrProto::OK);
        read->SetValue(out);
    } else {
        read->SetStatus(NKikimrProto::NODATA);
    }

    Ctx->Runtime->SingleSys()->Send(new IEventHandle(ActorId, Ctx->Edge, event.Release()));
}

void TUserActionProcessorFixture::WaitInfoRangeRequest()
{
    auto event = Ctx->Runtime->GrabEdgeEvent<TEvKeyValue::TEvRequest>();
    UNIT_ASSERT(event != nullptr);

    UNIT_ASSERT_VALUES_EQUAL(event->Record.CmdReadRangeSize(), 1);
}

void TUserActionProcessorFixture::SendInfoRangeResponse(ui32 partition,
                                                        const TVector<TCreateConsumerParams>& consumers)
{
    auto event = MakeHolder<TEvKeyValue::TEvResponse>();
    event->Record.SetStatus(NMsgBusProxy::MSTATUS_OK);

    auto read = event->Record.AddReadRangeResult();
    if (consumers.empty()) {
        read->SetStatus(NKikimrProto::NODATA);
    } else {
        read->SetStatus(NKikimrProto::OK);

        for (auto& c : consumers) {
            auto pair = read->AddPair();
            pair->SetStatus(NKikimrProto::OK);

            NPQ::TKeyPrefix key(NPQ::TKeyPrefix::TypeInfo, partition, NPQ::TKeyPrefix::MarkUser);
            key.Append(c.Consumer.data(), c.Consumer.size());
            pair->SetKey(key.Data(), key.Size());

            NKikimrPQ::TUserInfo userInfo;
            userInfo.SetOffset(c.Offset);
            userInfo.SetGeneration(c.Generation);
            userInfo.SetStep(c.Step);
            userInfo.SetSession(c.Session);
            userInfo.SetOffsetRewindSum(c.OffsetRewindSum);
            userInfo.SetReadRuleGeneration(c.ReadRuleGeneration);

            TString out;
            Y_PROTOBUF_SUPPRESS_NODISCARD userInfo.SerializeToString(&out);
            pair->SetValue(out);
        }
    }

    Ctx->Runtime->SingleSys()->Send(new IEventHandle(ActorId, Ctx->Edge, event.Release()));
}

void TUserActionProcessorFixture::WaitDataRangeRequest()
{
    auto event = Ctx->Runtime->GrabEdgeEvent<TEvKeyValue::TEvRequest>();
    UNIT_ASSERT(event != nullptr);

    UNIT_ASSERT_VALUES_EQUAL(event->Record.CmdReadRangeSize(), 1);
}

void TUserActionProcessorFixture::SendDataRangeResponse(ui64 begin, ui64 end)
{
    Y_VERIFY(begin <= end);

    auto event = MakeHolder<TEvKeyValue::TEvResponse>();
    event->Record.SetStatus(NMsgBusProxy::MSTATUS_OK);

    auto read = event->Record.AddReadRangeResult();
    read->SetStatus(NKikimrProto::OK);
    auto pair = read->AddPair();
    NPQ::TKey key(NPQ::TKeyPrefix::TypeData, 1, begin, 0, end - begin, 0);
    pair->SetStatus(NKikimrProto::OK);
    pair->SetKey(key.ToString());
    //pair->SetValueSize();
    pair->SetCreationUnixTime(0);

    Ctx->Runtime->SingleSys()->Send(new IEventHandle(ActorId, Ctx->Edge, event.Release()));
}

void TUserActionProcessorFixture::SendProposeTransactionRequest(ui32 partition,
                                                                ui64 begin, ui64 end,
                                                                const TString& client,
                                                                const TString& topic,
                                                                bool immediate,
                                                                ui64 txId)
{
    auto event = MakeHolder<TEvPersQueue::TEvProposeTransaction>();

    ActorIdToProto(Ctx->Edge, event->Record.MutableSource());
    auto* body = event->Record.MutableTxBody();
    auto* operation = body->MutableOperations()->Add();
    operation->SetPartitionId(partition);
    operation->SetBegin(begin);
    operation->SetEnd(end);
    operation->SetConsumer(client);
    operation->SetPath(topic);
    body->SetImmediate(immediate);
    event->Record.SetTxId(txId);

    Ctx->Runtime->SingleSys()->Send(new IEventHandle(ActorId, Ctx->Edge, event.Release()));
}

void TUserActionProcessorFixture::WaitProposeTransactionResponse(const TProposeTransactionResponseMatcher& matcher)
{
    auto event = Ctx->Runtime->GrabEdgeEvent<TEvPersQueue::TEvProposeTransactionResult>();
    UNIT_ASSERT(event != nullptr);

    if (matcher.TxId) {
        UNIT_ASSERT(event->Record.HasTxId());
        UNIT_ASSERT_VALUES_EQUAL(*matcher.TxId, event->Record.GetTxId());
    }

    if (matcher.Status) {
        UNIT_ASSERT(event->Record.HasStatus());
        UNIT_ASSERT(*matcher.Status == event->Record.GetStatus());
    }
}

void TUserActionProcessorFixture::SendCalcPredicate(ui64 step,
                                                    ui64 txId,
                                                    const TString& consumer,
                                                    ui64 begin,
                                                    ui64 end)
{
    auto event = MakeHolder<TEvPQ::TEvTxCalcPredicate>(step, txId);
    event->AddOperation(consumer, begin, end);

    Ctx->Runtime->SingleSys()->Send(new IEventHandle(ActorId, Ctx->Edge, event.Release()));
}

void TUserActionProcessorFixture::WaitCalcPredicateResult(const TCalcPredicateMatcher& matcher)
{
    auto event = Ctx->Runtime->GrabEdgeEvent<TEvPQ::TEvTxCalcPredicateResult>();
    UNIT_ASSERT(event != nullptr);

    if (matcher.Step) {
        UNIT_ASSERT_VALUES_EQUAL(*matcher.Step, event->Step);
    }
    if (matcher.TxId) {
        UNIT_ASSERT_VALUES_EQUAL(*matcher.TxId, event->TxId);
    }
    if (matcher.Partition) {
        UNIT_ASSERT_VALUES_EQUAL(*matcher.Partition, event->Partition);
    }
    if (matcher.Predicate) {
        UNIT_ASSERT_VALUES_EQUAL(*matcher.Predicate, event->Predicate);
    }
}

void TUserActionProcessorFixture::SendCommitTx(ui64 step, ui64 txId)
{
    auto event = MakeHolder<TEvPQ::TEvTxCommit>(step, txId);
    Ctx->Runtime->SingleSys()->Send(new IEventHandle(ActorId, Ctx->Edge, event.Release()));
}

void TUserActionProcessorFixture::SendRollbackTx(ui64 step, ui64 txId)
{
    auto event = MakeHolder<TEvPQ::TEvTxRollback>(step, txId);
    Ctx->Runtime->SingleSys()->Send(new IEventHandle(ActorId, Ctx->Edge, event.Release()));
}

void TUserActionProcessorFixture::WaitCommitTxDone(const TCommitTxDoneMatcher& matcher)
{
    auto event = Ctx->Runtime->GrabEdgeEvent<TEvPQ::TEvTxCommitDone>();
    UNIT_ASSERT(event != nullptr);

    if (matcher.Step) {
        UNIT_ASSERT_VALUES_EQUAL(*matcher.Step, event->Step);
    }
    if (matcher.TxId) {
        UNIT_ASSERT_VALUES_EQUAL(*matcher.TxId, event->TxId);
    }
    if (matcher.Partition) {
        UNIT_ASSERT_VALUES_EQUAL(*matcher.Partition, event->Partition);
    }
}

TTransaction TUserActionProcessorFixture::MakeTransaction(ui64 step, ui64 txId,
                                                          TString consumer,
                                                          ui64 begin, ui64 end,
                                                          TMaybe<bool> predicate)
{
    auto event = MakeSimpleShared<TEvPQ::TEvTxCalcPredicate>(step, txId);
    event->AddOperation(std::move(consumer), begin, end);

    return TTransaction(event, predicate);
}

Y_UNIT_TEST_F(Batching, TUserActionProcessorFixture)
{
    CreatePartition();

    SendCreateSession(4, "client-1", "session-id-1", 2, 3);

    WaitCmdWrite({.Count=2, .UserInfos={{0, {.Session = "session-id-1", .Offset=0, .Generation=2, .Step=3}}}});

    SendCreateSession(5, "client-2", "session-id-2", 4, 5);
    SendCreateSession(6, "client-3", "session-id-3", 6, 7);

    SendCmdWriteResponse(NMsgBusProxy::MSTATUS_OK);

    WaitProxyResponse({.Cookie=4});

    WaitCmdWrite({.Count=4, .UserInfos={
                 {0, {.Session = "session-id-2", .Offset=0, .Generation=4, .Step=5}},
                 {2, {.Session = "session-id-3", .Offset=0, .Generation=6, .Step=7}}
                 }});

    SendSetOffset(7, "client-1", 0, "session-id-1");
    SendCreateSession(8, "client-1", "session-id-2", 8, 9);
    SendSetOffset(9, "client-1", 0, "session-id-1");
    SendSetOffset(10, "client-1", 0, "session-id-2");
    SendCreateSession(11, "client-1", "session-id-3", 7, 10);

    SendCmdWriteResponse(NMsgBusProxy::MSTATUS_OK);

    WaitProxyResponse({.Cookie=5});
    WaitProxyResponse({.Cookie=6});

    WaitCmdWrite({.Count=2, .UserInfos={
                 {0, {.Session = "session-id-2", .Offset=0, .Generation=8, .Step=9}},
                 }});

    SendCmdWriteResponse(NMsgBusProxy::MSTATUS_OK);

    WaitProxyResponse({.Cookie=7, .Status=NMsgBusProxy::MSTATUS_OK});
    WaitProxyResponse({.Cookie=8, .Status=NMsgBusProxy::MSTATUS_OK});
    WaitErrorResponse({.Cookie=9, .ErrorCode=NPersQueue::NErrorCode::WRONG_COOKIE});
    WaitProxyResponse({.Cookie=10, .Status=NMsgBusProxy::MSTATUS_OK});
    WaitErrorResponse({.Cookie=11, .ErrorCode=NPersQueue::NErrorCode::WRONG_COOKIE});
}

Y_UNIT_TEST_F(SetOffset, TUserActionProcessorFixture)
{
    const ui32 partition = 0;
    const ui64 begin = 0;
    const ui64 end = 10;
    const TString client = "client";
    const TString session = "session";

    CreatePartition({.Partition=partition, .Begin=begin, .End=end});

    //
    // create session
    //
    CreateSession(client, session);

    //
    // regular commit (5 <= end)
    //
    SendSetOffset(1, client, 5, session);
    WaitCmdWrite({.Count=2, .UserInfos={{0, {.Session=session, .Offset=5}}}});
    SendCmdWriteResponse(NMsgBusProxy::MSTATUS_OK);
    WaitProxyResponse({.Cookie=1, .Status=NMsgBusProxy::MSTATUS_OK});

    //
    // offset is 5
    //
    SendGetOffset(2, client);
    WaitProxyResponse({.Cookie=2, .Status=NMsgBusProxy::MSTATUS_OK, .Offset=5});

    //
    // commit to back (1 < 5)
    //
    SendSetOffset(3, client, 1, session);
    WaitCmdWrite({.Count=2, .UserInfos={{0, {.Session=session, .Offset=5}}}});
    SendCmdWriteResponse(NMsgBusProxy::MSTATUS_OK);
    WaitProxyResponse({.Cookie=3, .Status=NMsgBusProxy::MSTATUS_OK});

    //
    // the offset has not changed
    //
    SendGetOffset(4, client);
    WaitProxyResponse({.Cookie=4, .Status=NMsgBusProxy::MSTATUS_OK, .Offset=5});

    //
    // commit to future (13 > end)
    //
    SendSetOffset(5, client, 13, session);
    WaitCmdWrite({.Count=2, .UserInfos={{0, {.Session=session, .Offset=end}}}});
    SendCmdWriteResponse(NMsgBusProxy::MSTATUS_OK);
    WaitProxyResponse({.Cookie=5, .Status=NMsgBusProxy::MSTATUS_OK});
}

Y_UNIT_TEST_F(CommitOffsetRanges, TUserActionProcessorFixture)
{
    const ui32 partition = 0;
    const ui64 begin = 0;
    const ui64 end = 10;
    const TString client = "client";
    const TString session = "session";

    CreatePartition({.Partition=partition, .Begin=begin, .End=end});

    //
    // create session
    //
    CreateSession(client, session);

    SendProposeTransactionRequest(partition,
                                  0, 2,  // 0 --> 2
                                  client,
                                  "topic-path",
                                  true,
                                  1);
    WaitCmdWrite({.Count=2, .UserInfos={{0, {.Session="", .Offset=2}}}});

    SendProposeTransactionRequest(partition,
                                  2, 0,          // begin > end
                                  client,
                                  "topic-path",
                                  true,
                                  2);
    SendProposeTransactionRequest(partition,
                                  4, 6,          // begin > client.end
                                  client,
                                  "topic-path",
                                  true,
                                  3);
    SendProposeTransactionRequest(partition,
                                  1, 4,          // begin < client.end
                                  client,
                                  "topic-path",
                                  true,
                                  4);
    SendProposeTransactionRequest(partition,
                                  2, 4,          // begin == client.end
                                  client,
                                  "topic-path",
                                  true,
                                  5);
    SendProposeTransactionRequest(partition,
                                  4, 13,         // end > partition.end
                                  client,
                                  "topic-path",
                                  true,
                                  6);

    SendCmdWriteResponse(NMsgBusProxy::MSTATUS_OK);
    WaitProposeTransactionResponse({.TxId=1, .Status=NKikimrPQ::TEvProposeTransactionResult::COMPLETE});

    WaitCmdWrite({.Count=2, .UserInfos={{0, {.Session="", .Offset=4}}}});
    SendCmdWriteResponse(NMsgBusProxy::MSTATUS_OK);

    WaitProposeTransactionResponse({.TxId=2, .Status=NKikimrPQ::TEvProposeTransactionResult::BAD_REQUEST});
    WaitProposeTransactionResponse({.TxId=3, .Status=NKikimrPQ::TEvProposeTransactionResult::ABORTED});
    WaitProposeTransactionResponse({.TxId=4, .Status=NKikimrPQ::TEvProposeTransactionResult::ABORTED});
    WaitProposeTransactionResponse({.TxId=5, .Status=NKikimrPQ::TEvProposeTransactionResult::COMPLETE});
    WaitProposeTransactionResponse({.TxId=6, .Status=NKikimrPQ::TEvProposeTransactionResult::BAD_REQUEST});

    SendGetOffset(6, client);
    WaitProxyResponse({.Cookie=6, .Offset=4});
}

Y_UNIT_TEST_F(CorrectRange_Commit, TUserActionProcessorFixture)
{
    const ui32 partition = 3;
    const ui64 begin = 0;
    const ui64 end = 10;
    const TString client = "client";
    const TString session = "session";

    const ui64 step = 12345;
    const ui64 txId = 67890;

    CreatePartition({.Partition=partition, .Begin=begin, .End=end, .PlanStep=step, .TxId=10000});
    CreateSession(client, session);

    SendCalcPredicate(step, txId, client, 0, 2);
    WaitCalcPredicateResult({.Step=step, .TxId=txId, .Partition=partition, .Predicate=true});

    SendCommitTx(step, txId);

    WaitCmdWrite({.Count=3, .PlanStep=step, .TxId=txId, .UserInfos={{1, {.Session="", .Offset=2}}}});
    SendCmdWriteResponse(NMsgBusProxy::MSTATUS_OK);

    WaitCommitTxDone({.TxId=txId, .Partition=partition});
}

Y_UNIT_TEST_F(CorrectRange_Multiple_Transactions, TUserActionProcessorFixture)
{
    const ui32 partition = 3;
    const ui64 begin = 0;
    const ui64 end = 10;
    const TString client = "client";
    const TString session = "session";

    const ui64 step = 12345;
    const ui64 txId_1 = 67890;
    const ui64 txId_2 = 67891;
    const ui64 txId_3 = 67892;

    CreatePartition({.Partition=partition, .Begin=begin, .End=end, .PlanStep=step, .TxId=10000});
    CreateSession(client, session);

    SendCalcPredicate(step, txId_1, client, 0, 1);
    WaitCalcPredicateResult({.Step=step, .TxId=txId_1, .Partition=partition, .Predicate=true});

    SendCalcPredicate(step, txId_2, client, 0, 2);
    SendCalcPredicate(step, txId_3, client, 0, 2);

    SendCommitTx(step, txId_1);

    WaitCalcPredicateResult({.Step=step, .TxId=txId_2, .Partition=partition, .Predicate=false});
    SendRollbackTx(step, txId_2);

    WaitCalcPredicateResult({.Step=step, .TxId=txId_3, .Partition=partition, .Predicate=false});
    SendRollbackTx(step, txId_3);

    WaitCmdWrite({.Count=3, .PlanStep=step, .TxId=txId_3, .UserInfos={{1, {.Session="", .Offset=1}}}});
    SendCmdWriteResponse(NMsgBusProxy::MSTATUS_OK);

    WaitCommitTxDone({.TxId=txId_1, .Partition=partition});
}

Y_UNIT_TEST_F(CorrectRange_Multiple_Consumers, TUserActionProcessorFixture)
{
    const ui32 partition = 3;
    const ui64 begin = 0;
    const ui64 end = 10;

    const ui64 step = 12345;
    const ui64 txId = 67890;

    CreatePartition({.Partition=partition, .Begin=begin, .End=end});
    CreateSession("client-1", "session-1");
    CreateSession("client-2", "session-2");

    SendSetOffset(1, "client-1", 3, "session-1");
    SendCalcPredicate(step, txId, "client-2", 0, 1);
    SendSetOffset(2, "client-1", 6, "session-1");

    WaitCmdWrite({.Count=2, .UserInfos={{0, {.Session="session-1", .Offset=3}}}});
    SendCmdWriteResponse(NMsgBusProxy::MSTATUS_OK);

    WaitProxyResponse({.Cookie=1, .Status=NMsgBusProxy::MSTATUS_OK});

    WaitCalcPredicateResult({.Step=step, .TxId=txId, .Partition=partition, .Predicate=true});
    SendCommitTx(step, txId);

    WaitCmdWrite({.Count=5, .UserInfos={
                 {1, {.Session="", .Offset=1}},
                 {3, {.Session="session-1", .Offset=6}}
                 }});
}

Y_UNIT_TEST_F(OldPlanStep, TUserActionProcessorFixture)
{
    const ui32 partition = 3;
    const ui64 begin = 0;
    const ui64 end = 10;

    const ui64 step = 12345;
    const ui64 txId = 67890;

    CreatePartition({.Partition=partition, .Begin=begin, .End=end, .PlanStep=99999, .TxId=55555});

    SendCommitTx(step, txId);
    WaitCommitTxDone({.TxId=txId, .Partition=partition});
}

Y_UNIT_TEST_F(AfterRestart_1, TUserActionProcessorFixture)
{
    const ui32 partition = 3;
    const ui64 begin = 0;
    const ui64 end = 10;
    const TString consumer = "client";
    const TString session = "session";

    const ui64 step = 12345;

    TVector<TTransaction> txs;
    txs.push_back(MakeTransaction(step, 11111, consumer, 0, 2, true));
    txs.push_back(MakeTransaction(step, 22222, consumer, 2, 4));

    CreatePartition({.Partition=partition,
                    .Begin=begin,
                    .End=end,
                    .PlanStep=step, .TxId=10000,
                    .Transactions=std::move(txs)},
                    {{.Consumer=consumer, .Offset=0, .Session=session}});

    SendCommitTx(step, 11111);

    WaitCalcPredicateResult({.Step=step, .TxId=22222, .Partition=partition, .Predicate=true});
    SendCommitTx(step, 22222);

    WaitCmdWrite({.Count=3, .PlanStep=step, .TxId=22222, .UserInfos={{1, {.Session="", .Offset=4}}}});
}

Y_UNIT_TEST_F(AfterRestart_2, TUserActionProcessorFixture)
{
    const ui32 partition = 3;
    const ui64 begin = 0;
    const ui64 end = 10;
    const TString consumer = "client";
    const TString session = "session";

    const ui64 step = 12345;

    TVector<TTransaction> txs;
    txs.push_back(MakeTransaction(step, 11111, consumer, 0, 2));
    txs.push_back(MakeTransaction(step, 22222, consumer, 2, 4));

    CreatePartition({.Partition=partition,
                    .Begin=begin,
                    .End=end,
                    .PlanStep=step, .TxId=10000,
                    .Transactions=std::move(txs)},
                    {{.Consumer=consumer, .Offset=0, .Session=session}});

    WaitCalcPredicateResult({.Step=step, .TxId=11111, .Partition=partition, .Predicate=true});
}

Y_UNIT_TEST_F(IncorrectRange, TUserActionProcessorFixture)
{
    const ui32 partition = 3;
    const ui64 begin = 0;
    const ui64 end = 10;
    const TString client = "client";
    const TString session = "session";

    const ui64 step = 12345;
    ui64 txId = 67890;

    CreatePartition({.Partition=partition, .Begin=begin, .End=end});
    CreateSession(client, session);

    SendCalcPredicate(step, txId, client, 4, 2);
    WaitCalcPredicateResult({.Step=step, .TxId=txId, .Partition=partition, .Predicate=false});
    SendRollbackTx(step, txId);

    WaitCmdWrite({.Count=1, .PlanStep=step, .TxId=txId});
    SendCmdWriteResponse(NMsgBusProxy::MSTATUS_OK);

    ++txId;

    SendCalcPredicate(step, txId, client, 2, 4);
    WaitCalcPredicateResult({.Step=step, .TxId=txId, .Partition=partition, .Predicate=false});
    SendRollbackTx(step, txId);

    WaitCmdWrite({.Count=1, .PlanStep=step, .TxId=txId});
    SendCmdWriteResponse(NMsgBusProxy::MSTATUS_OK);

    ++txId;

    SendCalcPredicate(step, txId, client, 0, 11);
    WaitCalcPredicateResult({.Step=step, .TxId=txId, .Partition=partition, .Predicate=false});
}

Y_UNIT_TEST_F(CorrectRange_Rollback, TUserActionProcessorFixture)
{
    const ui32 partition = 3;
    const ui64 begin = 0;
    const ui64 end = 10;
    const TString client = "client";
    const TString session = "session";

    const ui64 step = 12345;
    const ui64 txId_1 = 67890;
    const ui64 txId_2 = 67891;

    CreatePartition({.Partition=partition, .Begin=begin, .End=end});
    CreateSession(client, session);

    SendCalcPredicate(step, txId_1, client, 0, 2);
    WaitCalcPredicateResult({.Step=step, .TxId=txId_1, .Partition=partition, .Predicate=true});

    SendCalcPredicate(step, txId_2, client, 0, 5);
    SendRollbackTx(step, txId_1);

    WaitCalcPredicateResult({.Step=step, .TxId=txId_2, .Partition=partition, .Predicate=true});
}

} // Y_UNIT_TEST_SUITE(TUserActionProcessorTests)

} // namespace NKikimr::NPQ
