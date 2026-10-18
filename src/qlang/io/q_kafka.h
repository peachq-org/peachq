/* q_kafka — the `.kafka.i.*` natives (consumer and producer clients over a dlopen'd librdkafka), bound by
 * `.pq.load_natives`kafka`; the q surface is qlib/kafka.q. */
#ifndef QLANG_IO_Q_KAFKA_H
#define QLANG_IO_Q_KAFKA_H

void q_kafka_register(void);

#endif /* QLANG_IO_Q_KAFKA_H */
