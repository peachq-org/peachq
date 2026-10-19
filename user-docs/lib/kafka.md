# Kafka

`.kafka` connects q to a Kafka cluster. A connection is one handle that both consumes and publishes; a
subscription delivers one table per batch to a target you choose; a topic reads like a table. Underneath is
librdkafka, found at run time; nothing is downloaded.

Every function and option: [.kafka API](https://peachq.org/docs/api/kafka.q.html).

## Loading

The library is librdkafka, 2.0 or later: `PEACHQ_KAFKA_LIB` names it outright, else it is looked for in `$QHOME`,
beside `q`, then on the system (`librdkafka.so.1`). Without it the first open is refused with one line naming the fix,
and an older one (Ubuntu 22.04's is 1.8) with `'kafka: librdkafka 2.0 or later required (found 1.8.0)`:

```q
q).kafka.version[]
'kafka: librdkafka not found - set PEACHQ_KAFKA_LIB or install librdkafka 2.0 or later
```

The `-duckdb` download carries librdkafka 2.12.1 beside `q`, so it needs nothing installed. That build has no
GSSAPI (Kerberos): to use Kerberos, name your own librdkafka with `PEACHQ_KAFKA_LIB`. Windows has no Kafka yet.

## A connection is a handle

`hopen` of a `:pq:kafka:` alias opens it, `hclose` closes it, and `.pq.conns[]` lists it. The configuration is
librdkafka's, verbatim, as a dict of its keys to symbols, strings, numbers, booleans or symbol lists (joined with
commas) — every tuning guide applies as written, TLS and SASL included. A value with a hyphen (a host, `SCRAM-SHA-256`)
is a string. `group.id` is needed to subscribe; publishing needs only the brokers.

```q
q)h:hopen (`:pq:kafka:c1;`metadata.broker.list`group.id!`localhost:9092`myapp)
q)hopen (`:pq:kafka:c2;(enlist `no.such.key)!enlist `x)
'kafka: No such configuration property: "no.such.key"
```

## Publishing

`.kafka.pub` queues messages; `.kafka.flush` waits for their delivery. Applying the handle to a message publishes it
and waits; the negative form queues and returns.

```q
q).kafka.pub[h;`trades;"AAPL";"{\"sym\":\"AAPL\",\"px\":1.5}"]       / one message, key then payload
q).kafka.pub[h;`trades;`;("{\"sym\":\"IBM\",\"px\":1}";"{\"sym\":\"IBM\",\"px\":2}")]  / one per element, no key
q).kafka.flush[h;5000]
q)h (`trades;"MSFT";"{\"sym\":\"MSFT\",\"px\":2.5}")               / publish and wait
q)(neg h) (`trades;"{\"sym\":\"MSFT\",\"px\":3}")                  / fire and forget
q).kafka.pub[h;`trades;"1001";::]                                  / a tombstone for key 1001
```

A string or byte vector is ONE message; a list of them is one message each. The arguments are checked before
anything is queued, but a call that fails while queueing (`Local: Queue full`) or a `flush` that times out may
already have queued some messages, so a retry can double them. A message the broker never takes is a flush error and
reaches `.kafka.errcb`, never a silent success.

## Subscribing

`.kafka.sub[h;topic;target]` delivers every batch the event loop drains to the target, once per batch, and commits
the batch's offsets when the target returns. The target decides what a batch becomes:

- a **symbol** names a table the decoded JSON rows are upserted into, created when absent. Types come from the
  `types` option, else the table's meta, else a sniff of the batch; they convert values, they do not check them
  (`100.75` lands in a long column as `101`). A message must be one JSON object, or it is a decode reject;
- a **function of three arguments** `{[rows;rejects;misc] …}` receives the decoded rows, the batch's rejects and
  `misc`, and lands them itself;
- a **function of one argument** receives the raw batch: `topic partition offset msgtime msgkey headers data`.

Any target also comes as a pair `(target;opts)`: `types`, `meta` (message columns to keep), `decode`, `path`,
`tombstones`, `commit`, `max_per_wake` and the reject levers below — each checked when you subscribe.

```q
q).kafka.sub[h;`trades;`trade]
q).kafka.sub[h;`trades;(`trade;`meta`types!(`offset`msgtime;(enlist `px)!enlist "f"))]
q).kafka.sub[h;`trades;{[msgs] `raw upsert msgs}]
```

A tombstone (a null value, as Debezium sends after a delete) is a message, not an error; a symbol target rejects it
by default and every other target gets it as a row. `.kafka.subs h` lists the subscriptions with their tallies;
`.kafka.unsub[h;topic]` drops one.

Delivery runs on the main event loop, like a message on an IPC handle; nothing runs on another thread.

## When a message is bad

The default is strict. A message that fails to decode, or a target that signals, holds the whole batch uncommitted,
pauses the topic and calls `.kafka.errcb` — a poison message stops the stream where you can see it, and is never
dropped or redelivered in a loop. The default `.kafka.errcb` writes one line to stderr:

```q
kafka: decode trades partition 0 offset 5: parse: not JSON: "not json"
```

Subscribing again resumes the topic and the held batch comes again from its first offset. Two levers, named as in
`.csv.read`, change the default: `ignore_errors` drops a bad message and counts it; `store_rejects` keeps it in a
table (`reject_errors`, or `rejects_table`) keyed by `(topic;partition;offset)` with its error and bytes — replayable:

```q
q).kafka.sub[h;`trades;(`trade;(enlist `store_rejects)!enlist 1b)]
q)reject_errors
topic  partition offset msgtime                       msgkey error   bytes
----------------------------------------------------------------------------
trades 0         5      2026.10.16D13:44:17.443000000 0x     "parse: not JSON: \"not json\"" 0x6e6f74206a736f6e
```

Replace `.kafka.errcb` as you would `.z.pc`; if yours signals, the error it was given is still printed. librdkafka's own
errors (a broker down, a rebalance failing) arrive there too.

```q
q).kafka.errcb:{[h;code;reason] alert[code;reason]}
```

## A topic as a table

`` `:pq:kafka:c1:trades/ `` is the topic as an append-only table, the segment being the topic's name as Kafka spells
it (build one with a `-` with `` `$"…" ``). Underneath every read is `.kafka.read`: the topic to its high-water mark at
the call — bounded, never a tail that waits. `(topic;opts)` bounds it by `partition`, `offset` (from-to, a null end
open) and `msgtime`, and takes `.kafka.sub`'s decode options.

```q
q).kafka.read[h;(`trades;`partition`offset!(0;100 200))]
q).kafka.read[h;(`trades;(enlist `msgtime)!enlist .z.p-0D01)]
q)select sym,px from `:pq:kafka:c1:trades/ where partition=0, offset within 100 200
q)count select from `:pq:kafka:c1:trades/
q)meta `:pq:kafka:c1:trades/
q)`:pq:kafka:c1:trades/ upsert ([] sym:`AAPL`MSFT; px:1.5 2.5)  / publishes each row as JSON
```

`select` pushes its partition, offset and msgtime constraints into the bounds. `count` of the topic is `0N`: Kafka
knows its offsets, not its rows (bad messages, tombstones and compaction gaps all take offsets), so count a read. Every read is strict and changes no
state: a message that does not decode signals `'kafka` naming it and the way past it (`ignore_errors` or
`store_rejects` in `.kafka.read`'s options). `upsert` writes floats at full precision and times as ISO 8601.

## Committing

Delivery is at-least-once: an offset is committed only after its target has processed the message, so a process
that stops in between gets that message again. The library adds the +1, never you. The `commit` option says who
decides when:

- `` `auto `` (the default): each batch commits as its target returns clean, and `hclose` or exit commits the last
  delivered positions, so a restart starts after them.
- `` `manual ``: nothing commits by itself. Hand `.kafka.commit[h;x]` what was processed — rows with
  `topic partition offset`, or one partition's `misc` — and it commits synchronously.

`manual` is for a target that holds rows before it makes them durable. It buffers each batch and, after each write,
commits the rows that write saved:

```q
h:hopen (`:pq:kafka:buffered;`metadata.broker.list`group.id`auto.offset.reset!`localhost:9092`buffered`earliest)
buf:()
keep:{[rows;bad;misc] buf::buf,enlist rows}
.kafka.sub[h;`trades;(keep;`commit`meta!(`manual;`topic`partition`offset))]
flush:{[] if[count buf; rows:raze buf; `:trade upsert delete topic from rows; .kafka.commit[h;rows]; buf::()]}
.z.ts:{flush[]}
\t 5000
```

A target that must not double rows keys its table by `(partition;offset)`, as the ingest below does.

## Offsets by hand

`.kafka.offsets h` shows the assignment: `topic partition committed position low high lag`. It is empty until the
group assigns partitions, so caught up is a non-empty table whose lag is all 0:

```q
q)o:.kafka.offsets h
q)(count o) and all 0=o`lag
1b
```

`.kafka.assign` replaces the assignment with the partitions (and offsets) it names, to replay without a group;
`.kafka.read` never commits.

## When the broker, the group or the process moves

- **The broker restarts.** librdkafka reconnects; a subscription carries on, nothing lost or doubled. `.kafka.errcb`
  hears `_TRANSPORT` and `_ALL_BROKERS_DOWN`.
- **A consumer joins the group.** The partitions split and both deliver. A target that holds a batch past
  `max.poll.interval.ms` loses its membership: the commit is refused (told to `.kafka.errcb`) and the batch comes again
  to the next holder — so key tables by `(partition;offset)`.
- **The alias closes or the process exits.** `auto`'s last commits are made durable, only ever moving the group
  forward. A closing producer first delivers what it has queued, waiting up to `message.timeout.ms`; what it cannot
  deliver goes to `.kafka.errcb` at `hclose`, and is counted on one stderr line at exit.
- **One topic of several fails.** It holds and pauses alone; the other topics deliver and commit. A topic is a name:
  librdkafka's `^pattern` form is refused.

## A complete ingest

A consumer that lands JSON trades, keeps the bad ones, saves both tables as each batch lands, and stops when it has
caught up — so the next run starts where this one left off:

```q
h:hopen (`:pq:kafka:ingest;`metadata.broker.list`group.id`auto.offset.reset!`localhost:9092`ingest`earliest)
trade:([partition:`int$(); offset:`long$()] sym:`symbol$(); px:`float$(); qty:`long$(); rtime:`timestamp$())
rejects:([partition:`int$(); offset:`long$()] topic:`symbol$(); msgtime:`timestamp$(); msgkey:(); error:(); bytes:())
if[not ()~key `:trade; trade:get `:trade; rejects:get `:rejects]
land:{[rows;bad;misc] `trade upsert update rtime:.z.p from rows; `:trade set trade; `:rejects set rejects}
opts:`meta`types`store_rejects`rejects_table!(`partition`offset;`sym`px`qty!"sfj";1b;`rejects)
.kafka.sub[h;`trades;(land;opts)]
.z.ts:{o:.kafka.offsets h; if[(count o) and all 0=o`lag; exit 0]}
\t 1000
```

`land` saves before it returns, so the commit never runs ahead of the disk; a batch delivered again after a stop
overwrites its own rows, since both tables are keyed by `(partition;offset)`.

## Change data (Debezium)

Debezium publishes each row change as an envelope (`before`, `after`, `op`, under `payload` when schemas are kept)
and follows each delete with a tombstone. `path` lands the envelope, `meta` keeps the key and offset beside it, and a
three-argument target applies the changes in offset order:

```q
h:hopen (`:pq:kafka:cdc;`metadata.broker.list`group.id`auto.offset.reset!`localhost:9092`cdc`earliest)
customers:([id:`long$()] first_name:(); email:())
apply:{[rows;bad;misc]
  gone:exec offset from misc`tombstones;
  {[gone;row]
    pk:`long$(.j.k "c"$row`msgkey)[`payload;`id];
    $[(row[`offset] in gone) or "d"=first row`op; delete from `customers where id=pk;
      `customers upsert `id`first_name`email!(pk;row[`after]`first_name;row[`after]`email)]}[gone] each rows;}
.kafka.sub[h;`dbserver1.inventory.customers;(apply;`path`meta`types!(`payload;`offset`msgkey;`before`after!"**"))]
```

With the Avro converter a message is a `0x00` byte, a four-byte schema id and the Avro body; `decode` `bytes` lands it
framed, but `.kafka` has no Avro decoder:

```q
frames:{[rows;bad;misc] `framed upsert ([] offset:rows`offset; magic:rows[`data][;0]; schema:{0x0 sv 4#1_x} each rows`data; body:5_'rows`data);}
.kafka.sub[h;`dbserver1.inventory.customers;(frames;`decode`meta`tombstones!(`bytes;`offset;`skip))]
```
