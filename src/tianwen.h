#ifndef TIANWEN_H
#define TIANWEN_H
#include <inttypes.h>
#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <librdkafka/rdkafka.h>
#include <stdio.h>
#include <string.h>
#include <vector>

// #ifndef TIANWEN_H
// #define TIANWEN_H
#pragma once
#include <QObject>
#include <QString>
#include <thread>
#include "websocketthread.h"
#include <QObject>
#include <QThread>
#include <QString>
#include <QVector>
#include <QTimer>
#include <rdkafka.h>
#include <regex>
#include <string>
#include <iostream>
class WebSocketThread;

class TianWen : public QObject
{
    Q_OBJECT

public:
    explicit TianWen(QObject *parent = nullptr);
    ~TianWen();

    QVector<QString> getMessage(); // 获取消息
    std::tuple<std::string, std::string, std::string> parse_event(const std::string& event_text); // 解析事件
    static WebSocketThread *wsThread;    // WebSocket 线程
signals:
    void messageReceived(const QString &message); // 新消息信号，用于通知主线程

public slots:
    void pollKafkaMessages(); // 非阻塞式消息轮询函数

private:
    rd_kafka_t *rk;                      // Kafka consumer
    rd_kafka_topic_partition_list_t *topics_partitions; // 订阅的主题
    QTimer pollingTimer;                 // 定时器，用于非阻塞消息消费
    QVector<QString> messages;           // 消息存储
    
};

#endif // TIANWEN_H






