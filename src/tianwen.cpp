#include "tianwen.h"
#include <QDebug>

WebSocketThread *TianWen::wsThread = nullptr;

TianWen::TianWen(QObject *parent) : QObject(parent), rk(nullptr), topics_partitions(nullptr)
{
    char errstr[512];
    int err;

    // 配置 Kafka 客户端
    char rand_bytes[256], group_id[2 * sizeof(rand_bytes)] = {'\0'};
    BIO *b64 = BIO_new(BIO_f_base64());
    BIO *mem = BIO_new(BIO_s_mem());
    BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
    BIO_push(b64, mem);
    BIO_write(b64, rand_bytes, sizeof(rand_bytes) - 1);
    BIO_flush(b64);
    BIO_read(mem, group_id, sizeof(group_id) - 1);
    BIO_free_all(b64);

    char *conf_kv[][2] = {
        {"bootstrap.servers", "kafka.gcn.nasa.gov"},
        {"group.id", group_id},
        {"sasl.mechanisms", "OAUTHBEARER"},
        {"sasl.oauthbearer.client.id", "7mdvbl7kkokasg5r8sufrj8qp9"},
        {"sasl.oauthbearer.client.secret", "1e4e3lbf4jbsmjqajbj8if923o4m8flk4mfnqj677s9esjequ58t"},
        {"sasl.oauthbearer.method", "oidc"},
        {"sasl.oauthbearer.token.endpoint.url", "https://auth.gcn.nasa.gov/oauth2/token"},
        {"security.protocol", "sasl_ssl"}};

    static const char *topics[] = {
      "gcn.classic.text.AGILE_GRB_GROUND",
      "gcn.classic.text.AGILE_GRB_POS_TEST",
      "gcn.classic.text.AGILE_GRB_REFINED",
      "gcn.classic.text.AGILE_GRB_WAKEUP",
      "gcn.classic.text.AGILE_MCAL_ALERT",
      "gcn.classic.text.AGILE_POINTDIR",
      "gcn.classic.text.AGILE_TRANS",
      "gcn.classic.text.AMON_ICECUBE_COINC",
      "gcn.classic.text.AMON_ICECUBE_EHE",
      "gcn.classic.text.AMON_ICECUBE_HESE",
      "gcn.classic.text.AMON_NU_EM_COINC",
      "gcn.classic.text.CALET_GBM_FLT_LC",
      "gcn.classic.text.CALET_GBM_GND_LC",
      "gcn.classic.text.FERMI_GBM_ALERT",
      "gcn.classic.text.FERMI_GBM_FIN_POS",
      "gcn.classic.text.FERMI_GBM_FLT_POS",
      "gcn.classic.text.FERMI_GBM_GND_POS",
      "gcn.classic.text.FERMI_GBM_LC",
      "gcn.classic.text.FERMI_GBM_POS_TEST",
      "gcn.classic.text.FERMI_GBM_SUBTHRESH",
      "gcn.classic.text.FERMI_GBM_TRANS",
      "gcn.classic.text.FERMI_LAT_GND",
      "gcn.classic.text.FERMI_LAT_MONITOR",
      "gcn.classic.text.FERMI_LAT_OFFLINE",
      "gcn.classic.text.FERMI_LAT_POS_DIAG",
      "gcn.classic.text.FERMI_LAT_POS_INI",
      "gcn.classic.text.FERMI_LAT_POS_TEST",
      "gcn.classic.text.FERMI_LAT_POS_UPD",
      "gcn.classic.text.FERMI_LAT_TRANS",
      "gcn.classic.text.FERMI_POINTDIR",
      "gcn.classic.text.FERMI_SC_SLEW",
      "gcn.classic.text.GECAM_FLT",
      "gcn.classic.text.GECAM_GND",
      "gcn.classic.text.ICECUBE_ASTROTRACK_BRONZE",
      "gcn.classic.text.ICECUBE_ASTROTRACK_GOLD",
      "gcn.classic.text.ICECUBE_CASCADE",
      "gcn.classic.text.INTEGRAL_OFFLINE",
      "gcn.classic.text.INTEGRAL_POINTDIR",
      "gcn.classic.text.INTEGRAL_REFINED",
      "gcn.classic.text.INTEGRAL_SPIACS",
      "gcn.classic.text.INTEGRAL_WAKEUP",
      "gcn.classic.text.INTEGRAL_WEAK",
      "gcn.classic.text.IPN_POS",
      "gcn.classic.text.IPN_RAW",
      "gcn.classic.text.IPN_SEG",
      "gcn.classic.text.LVC_COUNTERPART",
      "gcn.classic.text.LVC_EARLY_WARNING",
      "gcn.classic.text.LVC_INITIAL",
      "gcn.classic.text.LVC_PRELIMINARY",
      "gcn.classic.text.LVC_RETRACTION",
      "gcn.classic.text.LVC_TEST",
      "gcn.classic.text.LVC_UPDATE",
      "gcn.classic.text.MAXI_KNOWN",
      "gcn.classic.text.MAXI_TEST",
      "gcn.classic.text.MAXI_UNKNOWN",
      "gcn.classic.text.SWIFT_ACTUAL_POINTDIR",
      "gcn.classic.text.SWIFT_BAT_ALARM_LONG",
      "gcn.classic.text.SWIFT_BAT_ALARM_SHORT",
      "gcn.classic.text.SWIFT_BAT_GRB_ALERT",
      "gcn.classic.text.SWIFT_BAT_GRB_LC",
      "gcn.classic.text.SWIFT_BAT_GRB_LC_PROC",
      "gcn.classic.text.SWIFT_BAT_GRB_POS_ACK",
      "gcn.classic.text.SWIFT_BAT_GRB_POS_NACK",
      "gcn.classic.text.SWIFT_BAT_GRB_POS_TEST",
      "gcn.classic.text.SWIFT_BAT_KNOWN_SRC",
      "gcn.classic.text.SWIFT_BAT_MONITOR",
      "gcn.classic.text.SWIFT_BAT_QL_POS",
      "gcn.classic.text.SWIFT_BAT_SCALEDMAP",
      "gcn.classic.text.SWIFT_BAT_SLEW_POS",
      "gcn.classic.text.SWIFT_BAT_SUB_THRESHOLD",
      "gcn.classic.text.SWIFT_BAT_SUBSUB",
      "gcn.classic.text.SWIFT_BAT_TRANS",
      "gcn.classic.text.SWIFT_FOM_OBS",
      "gcn.classic.text.SWIFT_FOM_PPT_ARG_ERR",
      "gcn.classic.text.SWIFT_FOM_SAFE_POINT",
      "gcn.classic.text.SWIFT_FOM_SLEW_ABORT",
      "gcn.classic.text.SWIFT_POINTDIR",
      "gcn.classic.text.SWIFT_SC_SLEW",
      "gcn.classic.text.SWIFT_TOO_FOM",
      "gcn.classic.text.SWIFT_TOO_SC_SLEW",
      "gcn.classic.text.SWIFT_UVOT_DBURST",
      "gcn.classic.text.SWIFT_UVOT_DBURST_PROC",
      "gcn.classic.text.SWIFT_UVOT_EMERGENCY",
      "gcn.classic.text.SWIFT_UVOT_FCHART",
      "gcn.classic.text.SWIFT_UVOT_FCHART_PROC",
      "gcn.classic.text.SWIFT_UVOT_POS",
      "gcn.classic.text.SWIFT_UVOT_POS_NACK",
      "gcn.classic.text.SWIFT_XRT_CENTROID",
      "gcn.classic.text.SWIFT_XRT_EMERGENCY",
      "gcn.classic.text.SWIFT_XRT_IMAGE",
      "gcn.classic.text.SWIFT_XRT_IMAGE_PROC",
      "gcn.classic.text.SWIFT_XRT_LC",
      "gcn.classic.text.SWIFT_XRT_POSITION",
      "gcn.classic.text.SWIFT_XRT_SPECTRUM",
      "gcn.classic.text.SWIFT_XRT_SPECTRUM_PROC",
      "gcn.classic.text.SWIFT_XRT_SPER",
      "gcn.classic.text.SWIFT_XRT_SPER_PROC",
      "gcn.classic.text.SWIFT_XRT_THRESHPIX",
      "gcn.classic.text.SWIFT_XRT_THRESHPIX_PROC",
      "gcn.classic.text.AAVSO",
      "gcn.classic.text.ALEXIS_SRC",
      "gcn.classic.text.BRAD_COORDS",
      "gcn.classic.text.CBAT",
      "gcn.classic.text.COINCIDENCE",
      "gcn.classic.text.COMPTEL_SRC",
      "gcn.classic.text.DOW_TOD",
      "gcn.classic.text.GRB_CNTRPART",
      "gcn.classic.text.GRB_COORDS",
      "gcn.classic.text.GRB_FINAL",
      "gcn.classic.text.GWHEN_COINC",
      "gcn.classic.text.HAWC_BURST_MONITOR",
      "gcn.classic.text.HUNTS_SRC",
      "gcn.classic.text.KONUS_LC",
      "gcn.classic.text.MAXBC",
      "gcn.classic.text.MILAGRO_POS",
      "gcn.classic.text.MOA",
      "gcn.classic.text.OGLE",
      "gcn.classic.text.SIMBADNED",
      "gcn.classic.text.SK_SN",
      "gcn.classic.text.SNEWS",
      "gcn.classic.text.SUZAKU_LC",
      "gcn.classic.text.TEST_COORDS"
        
        
        
        
        
        
        
        
        
        /* 省略其他主题以节省篇幅 */};

    static const int num_conf_kv = sizeof(conf_kv) / sizeof(*conf_kv);
    static const int num_topics = sizeof(topics) / sizeof(*topics);

    // 配置 Kafka 消费者
    rd_kafka_conf_t *conf = rd_kafka_conf_new();
    for (int i = 0; i < num_conf_kv; i++)
    {
        if (rd_kafka_conf_set(conf, conf_kv[i][0], conf_kv[i][1], errstr, sizeof(errstr)))
        {
            qCritical() << errstr;
            rd_kafka_conf_destroy(conf);
            return;
        }
    }

    rk = rd_kafka_new(RD_KAFKA_CONSUMER, conf, errstr, sizeof(errstr));
    if (!rk)
    {
        qCritical() << errstr;
        return;
    }

    topics_partitions = rd_kafka_topic_partition_list_new(num_topics);
    for (int i = 0; i < num_topics; i++)
        rd_kafka_topic_partition_list_add(topics_partitions, topics[i], RD_KAFKA_PARTITION_UA);

    err = rd_kafka_subscribe(rk, topics_partitions);
    if (err)
    {
      //  qCritical() << rd_kafka_err2str(err);
        rd_kafka_topic_partition_list_destroy(topics_partitions);
        rd_kafka_destroy(rk);
        rk = nullptr;
        return;
    }

    // 启动定时器
    connect(&pollingTimer, &QTimer::timeout, this, &TianWen::pollKafkaMessages);
    pollingTimer.start(10); // 每 100 毫秒轮询一次 Kafka 消息
}

TianWen::~TianWen()
{
    pollingTimer.stop();
    if (rk)
    {
        rd_kafka_consumer_close(rk);
        rd_kafka_destroy(rk);
    }
    if (topics_partitions)
        rd_kafka_topic_partition_list_destroy(topics_partitions);
}


void TianWen::pollKafkaMessages()
{
    if (!rk)
        return;

    rd_kafka_message_t *message = rd_kafka_consumer_poll(rk, 0); // 非阻塞轮询
    if (!message)
        return;

    if (message->err)
    {
        if (message->err == RD_KAFKA_RESP_ERR__PARTITION_EOF || message->err == RD_KAFKA_RESP_ERR_UNKNOWN_TOPIC_OR_PART)
        {
            qWarning() << rd_kafka_message_errstr(message);
        }
        else
        {
            qCritical() << rd_kafka_message_errstr(message);
            rd_kafka_message_destroy(message);
            return;
        }
    }
    else
    {
        QString payload = QString::fromUtf8(static_cast<char *>(message->payload), message->len);
        qDebug()<<"payloadneirong"<<payload;
        std::string notice_type, ra, dec;
        std::string event_text = payload.toStdString();
        std::tie(notice_type, ra, dec) = parse_event(event_text);
        if (ra != "0" && dec != "0" && notice_type != "") {
          qDebug()<<"TianWen:"+ QString::fromStdString(notice_type) + ":" + QString::fromStdString(ra) + ":" + QString::fromStdString(dec);
          emit wsThread->sendMessageToClient("TianWen:"+ QString::fromStdString(notice_type) + ":" + QString::fromStdString(ra) + ":" + QString::fromStdString(dec)); // 通知主线程
        }else{
          qDebug()<<"TianWen:0:0:0";
        }
    }

    rd_kafka_message_destroy(message);
}



std::tuple<std::string, std::string, std::string> TianWen::parse_event(const std::string& event_text) {
    std::smatch match;
    std::regex notice_type_regex("NOTICE_TYPE:\\s*(.*)");
    std::regex ra_regex("RA:\\s*(.*)d");
    std::regex dec_regex("DEC:\\s*(.*)d");

    std::string notice_type = "0", ra = "0", dec = "0";

    if (std::regex_search(event_text, match, notice_type_regex)) {
        notice_type = match.str(1);
    }

    if (std::regex_search(event_text, match, ra_regex)) {
        ra = match.str(1);
    }

    if (std::regex_search(event_text, match, dec_regex)) {
        dec = match.str(1);
    }

    std::cout << "Notice Type: " << notice_type << ", RA: " << ra << ", DEC: " << dec << std::endl;
    return std::make_tuple(notice_type, ra, dec);
}