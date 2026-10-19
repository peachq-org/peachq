/ Kafka from q: a client is a handle, a subscription delivers ONE table per batch to a target, a topic is a table
/ coordinate, and the same handle publishes bytes or rows.  hopen (`:pq:kafka:c1;cfg) opens the alias; cfg is
/ librdkafka's configuration verbatim, a dict of its keys (metadata.broker.list, group.id, ...) to symbols or strings,
/ judged at the open.  The alias makes its consumer the first time it subscribes, commits, assigns or asks for its
/ offsets, and its producer the first time it publishes or, with no client yet, asks for metadata; both come from cfg (a key
/ librdkafka scopes to the other role is left out of each).
/ .kafka.sub[h;`trades;`trade] decodes each batch's JSON into rows upserted into trade (types: explicit > trade's meta
/ > a sniff); .kafka.sub[h;`trades;{[msgs] ...}] hands the raw batch table (topic partition offset msgtime msgkey headers
/ data) to a function; either way the batch's offsets are committed once the target returns (or, with the option
/ commit `manual, when it hands .kafka.commit what it processed), and a batch that fails is held unread, the topic
/ paused and .kafka.errcb called - never a silent drop, never a redelivery loop.
/ `:pq:kafka:c1:trades/ is the topic as an append-only table: select reads it to its high-water mark at the call,
/ count get is 0N (the rows need a read: count select from it), meta and key decode a sample of its tail, and upsert
/ publishes rows as JSON.
/ h (`trades;msgkey;bytes) publishes one message and waits for its delivery; neg h is fire-and-forget.  hclose closes
/ the alias; .pq.conns[] lists it.
/ .
/ @eg h:hopen (`:pq:kafka:c1;`metadata.broker.list`group.id!`localhost:9092`myapp)
/ @eg .kafka.pub[h;`trades;"AAPL";"{\"sym\":\"AAPL\",\"px\":1.5}"]
/ @eg `:pq:kafka:c1:trades/ upsert ([] sym:`AAPL`MSFT; px:1.5 2.5)
/ @eg select from `:pq:kafka:c1:trades/ where partition=0, offset within 0 100
/ @eg count select from `:pq:kafka:c1:trades/
/ @eg get `:pq:kafka:c1:trades/
/ @eg .kafka.read[h;`trades]
/ @eg .kafka.read[h;(`trades;`partition`offset!(0;100 200))]
/ @eg .kafka.sub[h;`trades;`trade]
/ @eg .kafka.sub[h;`quotes;(`quote;`meta`store_rejects!(`topic`offset`msgtime;1b))]
/ @eg .kafka.subs h
/ @eg .kafka.subs[]
/ @eg .kafka.errcb:{[h;code;reason] -2 "kafka ",string[code]," ",reason`error;}

.pq.load_natives`kafka;

/ the subscriptions: the tally as a keyed table, and by (handle;topic) the target, its options and a topic's decoded
/ tail sample - dicts, because a general column collapses to the type of its first value
.kafka.i.subs:([handle:`symbol$(); topic:`symbol$()] decode:`symbol$(); messages:`long$(); rejected:`long$(); tombstones:`long$(); lastoffset:`long$(); paused:`boolean$());
.kafka.i.targets:(enlist (`;`))!enlist (::);
.kafka.i.opts:(enlist (`;`))!enlist (::);
.kafka.i.samples:(enlist (`;`))!enlist (::);
.kafka.i.OPTS:`types`meta`decode`ignore_errors`store_rejects`rejects_table`max_per_wake`tombstones`path`commit!(::;`symbol$();`json;0b;0b;`reject_errors;0N;`;::;`auto);
/ the options .kafka.read takes beside its three bounds: the decode half of .kafka.sub's
.kafka.i.READ_OPTS:`types`decode`ignore_errors`store_rejects`rejects_table`tombstones`path;
.kafka.i.TOMBSTONE:"tombstone: null value - the tombstones option (`row or `skip) lands it";
.kafka.i.META:`topic`partition`offset`msgtime`msgkey;
.kafka.i.REJECTS:flip `topic`partition`offset`msgtime`msgkey`error`bytes!(`symbol$();`int$();`long$();`timestamp$();();();());
/ The ms a bounded read waits for a quiet partition before it signals; the messages per partition a sniff samples.
.kafka.TIMEOUT:10000;
.kafka.SAMPLE:20480;

/ Called with every error an alias meets: a message that does not decode or a target that signals (code `decode or
/ `target), a refused commit, librdkafka's own errors; reason is `topic`partition`offset`error!(...), the message
/ fields null for a library error.  The default writes one line to stderr; replace it as you would .z.pc - a
/ reload of the library keeps the one you set.
.kafka.errcb:@[get;`.kafka.errcb;{[err] {[h;code;reason]
  -2 @[{[code;reason]
    at:$[null reason`topic; ""; " ",string[reason`topic]," partition ",string[reason`partition]," offset ",string reason`offset];
    "kafka: ",string[code],at,": ",$[10h=type reason`error; reason`error; -3!reason`error]}[code];reason;{[code;reason;err] "kafka: ",(-3!code)," ",-3!reason}[code;reason]];}}];
/ Called with librdkafka's statistics JSON (a string) every statistics.interval.ms, when that key is set.  The
/ default does nothing; a reload of the library keeps the one you set.
.kafka.statcb:@[get;`.kafka.statcb;{[err] {[h;json] }}];

/ the keys librdkafka 2.12 scopes to one role (its configuration table's C and P columns): each is left out of the
/ other role's client, which would log it as ignored; a key an older librdkafka lacks is only ever left out
.kafka.i.CONSUMER_KEYS:`group.id`group.instance.id`partition.assignment.strategy`session.timeout.ms`heartbeat.interval.ms,
  `group.protocol.type`group.protocol`group.remote.assignor`coordinator.query.interval.ms`max.poll.interval.ms,
  `enable.auto.commit`auto.commit.interval.ms`enable.auto.offset.store`queued.min.messages`queued.max.messages.kbytes,
  `fetch.wait.max.ms`fetch.queue.backoff.ms`fetch.message.max.bytes`max.partition.fetch.bytes`fetch.max.bytes,
  `fetch.min.bytes`fetch.error.backoff.ms`offset.store.method`isolation.level`enable.partition.eof`check.crcs,
  `auto.commit.enable`auto.offset.reset`offset.store.path`offset.store.sync.interval.ms`consume.callback.max.messages;
.kafka.i.PRODUCER_KEYS:`transactional.id`transaction.timeout.ms`enable.idempotence`enable.gapless.guarantee,
  `queue.buffering.max.messages`queue.buffering.max.kbytes`queue.buffering.max.ms`linger.ms`message.send.max.retries,
  `retries`queue.buffering.backpressure.threshold`compression.codec`compression.type`compression.level,
  `batch.num.messages`batch.size`delivery.report.only.error`sticky.partitioning.linger.ms`request.required.acks`acks,
  `request.timeout.ms`message.timeout.ms`delivery.timeout.ms`queuing.strategy`produce.offset.report`partitioner;

/ librdkafka's version, the first thing to check when a client does not open.
/ @return (string)
/ @throws kafka librdkafka not found - set PEACHQ_KAFKA_LIB or install it
/ @eg .kafka.version[]
.kafka.version:{[] .kafka.i.version[]};

/ Subscribe the alias to a topic.  The target decides what a batch becomes:
/ a symbol names a global table each batch's rows are upserted into (created when absent, upserted when keyed; its
/ meta fixes the types), decoded from JSON by default; every target takes options as (target;opts); a function of three arguments
/ {[rows;rejects;misc] ...} receives the decoded rows, the batch's reject records (under ignore_errors too) and misc
/ (`topic`partitions`offsets`rows`tombstones, the last a table of the batch's tombstones); a function of ONE argument receives the raw batch table of topic partition offset msgtime msgkey headers data
/ (msgkey and data byte vectors, headers a dict per message).  The batch's offsets are committed when the target returns,
/ unless the option commit is `manual.
/ A message that fails to decode, or a target that signals, leaves the batch uncommitted, rewinds and pauses the
/ topic, marks it paused in .kafka.subs and calls .kafka.errcb (code `decode or `target, the message named);
/ subscribing again resumes it, and the held batch comes again; subscribing again to a topic that is not paused
/ only changes its target, and unsub leaves a paused topic fetchable.  The alias's config needs group.id.
/ opts: types (a dict of column to type char, as .j.read takes; over the target's meta over a sniff of the batch);
/ meta (the message columns to keep beside the decoded ones, from `topic`partition`offset`msgtime`msgkey, default none);
/ decode (`json, `bytes or `string - the last two land one data column); ignore_errors (1b drops a message that
/ fails to decode); store_rejects (1b keeps it in the rejects table, with topic partition offset msgtime msgkey error
/ bytes - replayable, keyed by topic partition offset); rejects_table (symbol, default `reject_errors, implies
/ store_rejects); max_per_wake (the messages one wake of the consumer delivers at most); path (handed to .j.read
/ unchanged: the part of each message to land); tombstones (what a null value becomes: `row - typed nulls beside its
/ message columns, `:: in data for a raw batch - `skip - counted in .kafka.subs - or `reject; a symbol target's
/ default is `reject, every other target's `row); commit (`auto, the default: each batch commits as its target returns
/ clean, and hclose or exit commits the last of them synchronously; `manual: nothing commits until the target - or
/ anything later - hands .kafka.commit what it processed, and hclose or exit commits nothing more).  Options are
/ checked at the call: a bad one signals 'kafka naming it.
/ @param topic (symbol)
/ @param target (any) a symbol, (symbol;opts), (function;opts) or a function
/ @throws conn h is not an open alias
/ @eg .kafka.sub[h;`trades;`trade]
/ @eg .kafka.sub[h;`trades;(`trade;`types`meta!((enlist `px)!enlist "f";`offset`msgtime))]
/ @eg .kafka.sub[h;`trades;{[rows;rejects;misc] `trade upsert rows}]
/ @eg .kafka.sub[h;`trades;{[msgs] `raw upsert msgs}]
/ @eg .kafka.sub[h;`trades;({[rows;rejects;misc] `trade upsert rows; .kafka.commit[h;rows]};`commit`meta!(`manual;`topic`partition`offset))]
.kafka.sub:{[h;topic;target]
  if[not -11h=type topic; '`type];
  if["^"=first string topic; '"kafka: sub ",string[topic]," is a pattern, not a topic; .kafka.sub takes a topic's name"];
  sub:.kafka.i.sub_target target;
  h:.kafka.i.handle h;
  current:.kafka.i.subs (h;topic);
  / a live subscription only changes its target: resuming an unpaused partition would refetch it from the start
  $[null current`messages; .kafka.i.subscribe[h; distinct topic,exec topic from .kafka.i.subs where handle=h];
    current`paused; .kafka.i.resume[h;topic]; ::];
  if[not null sub[1]`max_per_wake; .kafka.i.max_wake[h;sub[1]`max_per_wake]];
  `.kafka.i.subs upsert (h;topic;sub 2;0^current`messages;0^current`rejected;0^current`tombstones;current`lastoffset;0b);
  .kafka.i.targets[(h;topic)]:sub 0;
  .kafka.i.opts[(h;topic)]:sub 1;
  .kafka.i.samples:(enlist (h;topic)) _ .kafka.i.samples;
  };

/ the target argument of .kafka.sub as (target; options; decode), the decode `batch for the raw form
.kafka.i.sub_target:{[target]
  opts:.kafka.i.OPTS;
  if[0h=type target;
    if[not 2=count target; '`length];
    if[not 99h=type target 1; '`type];
    .kafka.i.options[key opts;target 1];
    opts:opts,target 1;
    opts[`meta]:(),opts`meta;
    if[`rejects_table in key target 1; opts[`store_rejects]:1b];
    target:target 0];
  symbol:-11h=type target;
  if[not symbol or 100h<=type target; '`type];
  arity:$[symbol; 0; .kafka.i.rank target];
  if[not arity in 0 1 3; '`rank];
  if[null opts`tombstones; opts[`tombstones]:$[symbol; `reject; `row]];
  if[(arity=1) and opts[`tombstones]=`reject; '"kafka: option tombstones: a one-argument target gets the raw batch, so `row or `skip"];
  (target;opts;$[arity=1; `batch; opts`decode])};
/ options as given, checked against the names allowed and each one's own shape; a bad one is named
.kafka.i.options:{[allowed;opts]
  if[count opts; if[not 11h=type key opts; '`type]];
  unknown:(key opts) except allowed;
  if[count unknown; '"kafka: unknown option ",string[first unknown],"; the options are ",", " sv string allowed];
  bad:{[name;msg] '"kafka: option ",string[name],": ",msg};
  if[`meta in key opts; if[not (11h=type (),opts`meta) and all ((),opts`meta) in .kafka.i.META; bad[`meta;"symbols from ",", " sv string .kafka.i.META]]];
  if[`types in key opts; if[not (opts[`types]~(::)) or 99h=type opts`types; bad[`types;"a dict of column to type char"]]];
  if[`decode in key opts; if[not opts[`decode] in `json`bytes`string; bad[`decode;"`json, `bytes or `string"]]];
  {[bad;opts;name] if[name in key opts; if[not -1h=type opts name; bad[name;"a boolean"]]]}[bad;opts] each `ignore_errors`store_rejects;
  if[`rejects_table in key opts; if[not -11h=type opts`rejects_table; bad[`rejects_table;"a symbol"]]];
  if[`max_per_wake in key opts; if[not (type opts`max_per_wake) in -5 -6 -7h; bad[`max_per_wake;"an integer"]]];
  if[`tombstones in key opts; if[not opts[`tombstones] in `row`skip`reject; bad[`tombstones;"`row, `skip or `reject"]]];
  if[`path in key opts; if[not (type opts`path) in -11 11 -7 7 0h; bad[`path;"a symbol, a long or a list of them"]]];
  if[`commit in key opts; if[not opts[`commit] in `auto`manual; bad[`commit;"`auto or `manual"]]];
  };

/ Drop a topic from the alias's subscription.
/ @eg .kafka.unsub[h;`trades]
.kafka.unsub:{[h;topic]
  h:.kafka.i.handle h;
  / librdkafka keeps a partition's pause across a resubscribe, so a later sub would never fetch it
  if[.kafka.i.subs[(h;topic);`paused]; .kafka.i.resume[h;topic]];
  .kafka.i.subs:(enlist (h;topic)) _ .kafka.i.subs;
  .kafka.i.targets:(enlist (h;topic)) _ .kafka.i.targets;
  .kafka.i.opts:(enlist (h;topic)) _ .kafka.i.opts;
  .kafka.i.samples:(enlist (h;topic)) _ .kafka.i.samples;
  .kafka.i.subscribe[h; exec topic from .kafka.i.subs where handle=h];
  };

/ The subscriptions of an alias (h) or of every alias ([]): topic target decode messages rejected tombstones
/ lastoffset paused, with the handle first when every alias is listed.  decode is json, bytes or string, or batch for
/ a one-argument target (it gets the raw batch); tombstones counts the null values the tombstones option `skip passed by.
/ @eg .kafka.subs h
.kafka.subs:{[h]
  subs:0!.kafka.i.subs;
  if[not h~(::); subs:select from subs where handle=.kafka.i.handle h];
  targets:$[count subs; .kafka.i.targets each flip (subs`handle;subs`topic); ()];
  subs:`handle`topic`target xcols update target:targets from subs;
  $[h~(::); subs; delete handle from subs]};

/ Publish to a topic: one message, or one per element when data is a list (key then `:: for none, one key for all,
/ or a list of keys of the same count).  The messages are queued; .kafka.flush or a sync apply h (topic;msgkey;data)
/ waits for delivery.
/ @param msgkey (any) a string, byte vector or symbol, or `:: for no key
/ @param data (any) a string or byte vector (ONE message), :: (a tombstone: Kafka's null value), or a list of them;
/ the whole call is checked before anything is queued, and () is 'kafka (nothing to publish)
/ @eg .kafka.pub[h;`trades;"AAPL";"{\"sym\":\"AAPL\",\"px\":1.5}"]
/ @eg .kafka.pub[h;`trades;`;("{\"sym\":\"IBM\",\"px\":1}";"{\"sym\":\"IBM\",\"px\":2}")]
.kafka.pub:{[h;topic;msgkey;data] .kafka.i.produce[h;topic;msgkey;.kafka.i.payloads[msgkey;data]]};

/ the whole of a publish checked before any of it is queued: one payload (a string, a byte vector, or :: for a
/ tombstone) or a non-empty list of them, and a key per payload when keys are a list
.kafka.i.payloads:{[msgkey;data]
  one:{[item] (item~(::)) or (type item) in -10 -4 4 10h};
  if[one data; :data];
  if[not 0h=type data; '"kafka: pub data is of type ",(-3!type data),", not a string, a byte vector, :: or a list of them"];
  if[0=count data; '"kafka: pub has nothing to publish: data is ()"];
  bad:where not one each data;
  if[count bad; '"kafka: pub data item ",string[first bad]," is of type ",(-3!type data first bad),
    " - each item is a string, a byte vector or :: (a tombstone)"];
  if[((type msgkey) in 0 11h) and not count[msgkey]=count data;
    '"kafka: pub has ",string[count msgkey]," keys for ",string[count data]," payloads"];
  data};

/ Wait up to ms for every message the alias has queued to be delivered.
/ @throws kafka Local: Timed out when some are still queued
/ @eg .kafka.flush[h;5000]
.kafka.flush:{[h;ms] .kafka.i.flush[h;ms]};

/ The alias's assigned partitions: topic partition committed position low high lag.  position is the next offset the
/ alias reads (the delivered position, else the committed one, else low) and lag is high-position.  The table is empty
/ until the group assigns the partitions, in the first wakes after .kafka.sub; caught up is a non-empty table whose
/ lag is all 0.
/ @eg .kafka.offsets h
.kafka.offsets:{[h] .kafka.i.offsets h};

/ Commit what a target processed, synchronously: x is its rows (any table with topic partition offset - the meta
/ option keeps them) or the misc of a batch from one partition.  Each partition's commit is the offset after the last
/ one x holds: the +1 is the library's, never yours.  For a subscription with commit `manual, which never commits by
/ itself; under `auto every batch commits as its target returns.
/ @param x (any) the processed rows (a table), or a batch's misc (a dict)
/ @throws type x is neither, or a misc of several partitions (which cannot say where each one ends)
/ @throws kafka the broker refused the commit: the partition is named
/ @eg .kafka.commit[h;([] topic:enlist `trades; partition:enlist 0i; offset:enlist 0)]
.kafka.commit:{[h;x]
  offsets:.kafka.i.next .kafka.i.processed x;
  if[count offsets; .kafka.i.commit[h;offsets;0b]];
  };
/ the topic partition offset rows of x, a table or a batch's misc, or a 'type that says what x is
.kafka.i.processed:{[x]
  wrong:{[what] '"type: .kafka.commit x ",what,"; it takes the rows a target processed (with topic partition offset) or a one-partition batch's misc"};
  if[99h=type x; if[98h=type key x; x:0!x]];
  if[99h=type x; if[all `topic`partitions`offsets in key x;
    if[not 1=count x`partitions; wrong["is a misc of ",string[count x`partitions]," partitions"]];
    :([] topic:enlist x`topic; partition:"i"$x`partitions; offset:enlist last x`offsets)]];
  if[not 98h=type x; wrong["is of type ",-3!type x]];
  missing:`topic`partition`offset except cols x;
  if[count missing; wrong["has no column ",", " sv string missing]];
  if[any (null x`offset) or 0>x`offset; wrong["has a null or negative offset"]];
  x};
/ the offset each partition reads next: the one after the last processed
.kafka.i.next:{[rows] 0!select offset:1+max offset by topic,partition from rows};

/ Assign partitions to the alias by hand instead of a group subscription: a dict of topic to partitions, a table of
/ topic partition [offset], or `:: to clear the assignment.
/ @eg .kafka.assign[h;(enlist `trades)!enlist 0 1]
.kafka.assign:{[h;parts]
  if[99h=type parts; parts:raze {[topic;ids] ([] topic:count[ids]#topic; partition:"i"$(),ids)}'[key parts; value parts]];
  .kafka.i.assign[h;parts]};

/ The broker's topics as a table of topic partition leader replicas low high; a topic deleted while it is listed is
/ left out.
/ @eg .kafka.metadata h
.kafka.metadata:{[h] .kafka.i.metadata h};

/ Read a topic to its high-water mark at the call: every message, decoded as .kafka.sub decodes the topic (JSON when
/ unsubscribed), message columns first.  (topic;opts) narrows or relaxes it.  Three keys bound the read: partition (an
/ int atom or list), offset (a from-to pair, inclusive as within; a null end is open) and msgtime (the earliest time);
/ anything beyond them is a select.  The rest are .kafka.sub's decode options under the same names: types, decode,
/ ignore_errors, store_rejects, rejects_table, tombstones (default `row here) and path.  A read is strict: a message that does not decode signals 'kafka naming
/ it, unless ignore_errors skips it or store_rejects keeps it in the rejects table (keyed by topic partition offset).
/ Any other key, or a bad value, signals 'kafka naming the option.
/ @eg .kafka.read[h;`trades]
/ @eg .kafka.read[h;(`trades;`partition`offset!(0;100 200))]
/ @eg .kafka.read[h;(`trades;(enlist `ignore_errors)!enlist 1b)]
.kafka.read:{[h;spec]
  if[-11h=type spec; spec:(spec;()!())];
  if[not (0h=type spec) and 2=count spec; '`type];
  topic:spec 0; bounds:spec 1;
  if[not (-11h=type topic) and 99h=type bounds; '`type];
  .kafka.i.options[`partition`offset`msgtime,.kafka.i.READ_OPTS;bounds];
  given:(key[bounds] except `partition`offset`msgtime)#bounds;
  parts:$[`partition in key bounds; bounds`partition; ::];
  if[not (parts~(::)) or (type parts) in -6 6 -7 7h; '`type];
  span:$[`offset in key bounds; "j"$bounds`offset; 0N 0N];
  if[not (7h=type span) and 2=count span; '`type];
  since:$[`msgtime in key bounds; bounds`msgtime; 0Np];
  if[not -12h=type since; '`type];
  lo:$[not null span 0; 0|span 0; not null since; since; 0];
  hi:$[null span 1; ::; 1+span 1];
  rows:.kafka.i.decoded[h;topic;.kafka.i.read[h;topic;$[parts~(::); ::; "i"$(),parts];lo;hi;.kafka.TIMEOUT];given];
  if[not parts~(::); rows:rows where rows[`partition] in "i"$(),parts];
  if[not null span 0; rows:rows where rows[`offset]>=span 0];
  if[not null span 1; rows:rows where rows[`offset]<=span 1];
  if[not null since; rows:rows where rows[`msgtime]>=since];
  rows};

/ a symbol list value is spelled as librdkafka writes a list: joined with commas
.kafka.i.open:{[alias;cfg]
  if[not 99h=type cfg; :.kafka.i.new[alias;cfg;cfg]];
  cfg:@[cfg;where 11h=type each cfg;{[names] "," sv string names}];
  .kafka.i.new[alias;.kafka.i.PRODUCER_KEYS _ cfg;.kafka.i.CONSUMER_KEYS _ cfg]};
.kafka.i.close:{[token]
  h:.kafka.i.handle token;
  delete from `.kafka.i.subs where handle=h;
  keys_of:{[dict;h] (key dict) where (key dict)[;0]=h};
  .kafka.i.targets:keys_of[.kafka.i.targets;h] _ .kafka.i.targets;
  .kafka.i.opts:keys_of[.kafka.i.opts;h] _ .kafka.i.opts;
  .kafka.i.samples:keys_of[.kafka.i.samples;h] _ .kafka.i.samples;
  .kafka.i.destroy token};
/ @ignore
.kafka.call:{[token;msg;sync]
  if[not 0h=type msg; '`type];
  if[not count[msg] in 2 3; '`length];
  msgkey:$[3=count msg; msg 1; ::];
  .kafka.i.produce[token;msg 0;msgkey;.kafka.i.payloads[msgkey;last msg]];
  if[sync; .kafka.i.flush[token;.kafka.TIMEOUT]];
  };

.kafka.i.run:{[target;msgs] @[{[target;batch] (1b;target batch)}[target]; msgs; {[err] (0b;err)}]};
.kafka.i.deliver:{[h;batch] {[h;batch;topic] @[.kafka.i.deliver_topic[h;batch];topic;.kafka.i.astray[h;batch;topic]]}[h;batch] each distinct batch`topic; };
/ a signal out of one topic's delivery holds that topic alone, and says so on stderr
.kafka.i.astray:{[h;batch;topic;err]
  -2 "kafka: ",string[topic],": '",err;
  if[not (h;topic) in key .kafka.i.subs; :(::)];
  if[.kafka.i.subs[(h;topic);`paused]; :(::)];
  msgs:batch where batch[`topic]=topic;
  @[.kafka.i.hold[h];0!select offset:min offset by topic,partition from msgs;{[err] -2 "kafka: hold: '",err}];
  .kafka.i.subs[(h;topic);`paused]:1b};
/ errcb told; a signal out of it is one stderr line carrying what it was told and its own error
.kafka.i.tell:{[h;code;reason] if[`errcb in key `.kafka; @[.kafka.errcb[h;code];reason;{[code;reason;err] -2 "kafka: ",string[code]," ",(-3!reason),"; .kafka.errcb failed: '",err}[code;reason]]]};
.kafka.i.deliver_topic:{[h;batch;topic]
  sub:.kafka.i.subs (h;topic);
  msgs:batch where batch[`topic]=topic;
  if[null sub`messages;
    :.kafka.i.tell[h;`target;`topic`partition`offset`error!(topic;first msgs`partition;first msgs`offset;"no subscription takes this topic: its messages are neither delivered nor committed")]];
  if[sub`paused; :.kafka.i.hold[h; 0!select offset:min offset by topic,partition from msgs]];
  raw:sub[`decode]=`batch;
  result:$[raw; .kafka.i.run[.kafka.i.raw[h;topic]; msgs]; .kafka.i.run[.kafka.i.land[h;topic;sub;]; msgs]];
  / a target that unsubscribed or closed the alias ended the subscription: no commit, no tally
  if[not (h;topic) in key .kafka.i.subs; :(::)];
  if[result 0;
    if[`auto=.kafka.i.opts[(h;topic);`commit]; .kafka.i.commit[h;.kafka.i.next msgs;1b]];
    tally:$[raw; (0;$[`skip=.kafka.i.opts[(h;topic);`tombstones]; sum (::)~/:msgs`data; 0]); result 1];
    .kafka.i.subs[(h;topic);`messages`rejected`tombstones`lastoffset]:(sub[`messages]+count msgs; sub[`rejected]+tally 0; sub[`tombstones]+tally 1; max msgs`offset);
    :(::)];
  .kafka.i.hold[h; 0!select offset:min offset by topic,partition from msgs];
  .kafka.i.subs[(h;topic);`paused]:1b;
  undecoded:result[1]~"decode";
  bad:$[undecoded; .kafka.i.failed; first msgs];
  reason:`topic`partition`offset`error!(topic;bad`partition;bad`offset;$[undecoded;bad`error;result 1]);
  .kafka.i.tell[h;$[undecoded;`decode;`target];reason];
  };
/ a one-argument target's batch: tombstones skipped when asked, `::` in data otherwise
.kafka.i.raw:{[h;topic;msgs]
  if[`skip=.kafka.i.opts[(h;topic);`tombstones]; msgs:msgs where not (::)~/:msgs`data];
  (.kafka.i.targets (h;topic)) msgs};
.kafka.i.land:{[h;topic;sub;msgs]
  opts:.kafka.i.opts (h;topic);
  target:.kafka.i.targets (h;topic);
  decoded:.kafka.i.decode[msgs;sub`decode;opts;target];
  rejects:decoded 1;
  if[count rejects;
    if[not any opts`ignore_errors`store_rejects; .kafka.i.failed::first rejects; '`decode];
    if[opts`store_rejects; .kafka.i.keep[opts`rejects_table;rejects]]];
  tombs:decoded 2;
  misc:`topic`partitions`offsets`rows`tombstones!(topic;distinct msgs`partition;(min;max)@\:msgs`offset;count decoded 0;tombs);
  $[-11h=type target; if[count decoded 0; target upsert decoded 0]; target[decoded 0;rejects;misc]];
  (count rejects;$[`skip=opts`tombstones; count tombs; 0])};
/ (rows; rejects; tombstones): a null value (a tombstone) is a message, by the tombstones option a row of typed nulls
/ beside its message columns (data `:: in a raw decode), skipped, or a reject; an empty value is a decode reject
.kafka.i.decode:{[msgs;dec;opts;target]
  null_at:where (::)~/:msgs`data;
  tombs:.kafka.i.META#msgs null_at;
  mode:opts`tombstones;
  inline:(dec in `bytes`string) and mode=`row;
  live:$[inline; til count msgs; (til count msgs) except null_at];
  part:msgs live;
  rejects:.kafka.i.REJECTS;
  ok:til count part;
  rows:$[dec=`bytes; ([] data:part`data); dec=`string; ([] data:{[v] $[(::)~v; v; "c"$v]} each part`data); .kafka.i.json[part;opts;target]];
  if[dec=`json; ok:ok except rows 1; if[count rows 1; rejects:.kafka.i.rejects[part;rows 1;rows 2]]; rows:rows 0];
  if[not 98h=type rows; rows:()];
  if[not count[rows]=count ok; .kafka.i.failed::first part; '`decode];
  at:live ok;
  if[(mode=`reject) and count null_at; rejects,:.kafka.i.rejects[msgs;null_at;count[null_at]#enlist .kafka.i.TOMBSTONE]];
  if[(mode=`row) and (not inline) and count null_at;
    / a batch of nothing but tombstones has no decoded columns: the types asked for give the row its shape
    blank:$[98h=type rows; {[col] $[0h=type col; $[(count col) and 10h=type first col; ""; ::]; first 0#col]} each flip rows;
      99h=type opts`types; {[char] $[char="*"; ::; char="C"; ""; first char$()]} each opts`types; ()!()];
    if[(not 98h=type rows) and count blank; rows:0#enlist blank];
    rows:$[count blank; rows,count[null_at]#enlist blank; rows];
    at,:null_at;
    order:iasc at; at:at order;
    if[count blank; rows:rows order]];
  meta_cols:opts`meta;
  if[(count meta_cols) and count at; rows:$[98h=type rows; (meta_cols#msgs at),'rows; meta_cols#msgs at]];
  (rows;`partition`offset xasc rejects;tombs)};
.kafka.i.rejects:{[msgs;bad;errs] (.kafka.i.META#msgs bad),'([] error:errs; bytes:msgs[bad;`data])};
/ the batch as JSON lines through .j.read's target seam: a blank message is a reject .j.read would skip, and a
/ batch it refuses whole (nothing in it parses) is decoded a line at a time so each message's verdict is its own
.kafka.i.json:{[msgs;opts;target]
  lines:{[line] @[line;where line in "\r\n";:;" "]} each "c"$'msgs`data;
  blank:where 0=count each trim each lines;
  / without a path a record is a JSON object: any other value is a reject, not a json column for the batch
  flat:$[(::)~opts`path; (where not "{"=first each trim each lines) except blank; `long$()];
  first_errs:(count[blank]#`empty),{[line] $[`bad~@[.j.k;line;{[err] `bad}]; `parse; `type]} each lines flat;
  blank,:flat;
  good:(til count lines) except blank;
  levers:`ignore_errors`store_rejects`rejects_table`sample_size!(1b;1b;`.kafka.i.rj;1|count good);
  if[not (::)~opts`path; levers[`path]:opts`path];
  .kafka.i.rj:();
  .kafka.i.oracle[target;opts`meta];
  ok:(0<count good) and @[{[lines;types;levers] .j.read[lines;`.kafka.i.tmp;types;levers]; 1b}[lines good;opts`types;]; levers; {[err] 0b}];
  / .j.read decides records on the whole text, so under a path one null where a record should be makes the batch a
  / single json column: then each message is its own document, and that one the reject
  if[ok and not (::)~opts`path; if[(enlist `json)~cols .kafka.i.decoded_rows[]; ok:0b]];
  / a message of two documents lands two rows: then too each message is read alone
  if[ok; ok:count[.kafka.i.decoded_rows[]]=count[good]-count .kafka.i.rj];
  if[ok;
    bad:good (.kafka.i.rj`line)-1;
    :(.kafka.i.decoded_rows[]; blank,bad; .kafka.i.why[lines;blank,bad;first_errs,.kafka.i.rj`error;opts;target])];
  .kafka.i.oracle[target;opts`meta];
  bad:(); errs:(); idx:0;
  while[idx<count good;
    .kafka.i.rj:();
    / the first message's load fixes the schema; the rest land in it, as one load's sample would have fixed it
    types:$[`tmp in key `.kafka.i; ::; opts`types];
    before:count .kafka.i.decoded_rows[]; had:`tmp in key `.kafka.i;
    err:@[{[types;levers;line] .j.read[enlist line;`.kafka.i.tmp;types;levers]; ""}[types;levers]; lines good idx; {[err] err}];
    if[(0=count[err]+count .kafka.i.rj) and not 1=count[.kafka.i.decoded_rows[]]-before;
      $[had; .kafka.i.tmp:before#.kafka.i.tmp; ![`.kafka.i;();0b;enlist `tmp]]; err:"many"];
    if[(count err) or count .kafka.i.rj; bad,:good idx; errs,:$[count err; `$err; first .kafka.i.rj`error]];
    idx+:1];
  (.kafka.i.decoded_rows[]; blank,bad; .kafka.i.why[lines;blank,bad;first_errs,errs;opts;target])};
/ each reject's reason in words: the loader's one-word verdict, then what in the message earned it
.kafka.i.why:{[lines;idx;errs;opts;target]
  existing:$[-11h=type target; @[value;target;{[err] ::}]; ::];
  wanted:$[(type existing) in 98 99h; exec c!t from meta 0!existing; ()!()];
  if[99h=type opts`types; wanted,:opts`types];
  wanted:(key[wanted] except .kafka.i.META)#wanted;
  path:opts`path;
  {[wanted;path;line;err]
    text:-3!50 sublist line;
    if[err~`empty; :"empty: a blank message"];
    if[err~`parse; :"parse: not JSON: ",text];
    if[err~`many; :"parse: more than one JSON document: ",text];
    fields:@[.j.k;line;{[e] ::}];
    if[not path~(::); fields:@[{[fields;path] fields . (),path};(fields;path);{[e] ::}]];
    if[not 99h=type fields; :string[err],": not a JSON object",$[path~(::); ""; " at the path"],": ",text];
    if[err~`mismatch; :"mismatch: fields ",(" " sv string key fields)," are not the columns ",(" " sv string key wanted)];
    absent:(key wanted) except key fields;
    if[count absent; :"type: ",(" " sv string absent)," missing, wanted ",(" " sv string key wanted)];
    wrong:{[fields;col;want] got:fields col;
      $[10h=type got; (not want in "sC*") and null (upper want)$got; want in "bhijef"; not (type got) in -1 -9h; 0b]}[fields]'[key wanted;value wanted];
    if[any wrong; col:(key wanted) first where wrong; :"type: ",string[col]," is ",(.j.j fields col),", wanted ",wanted col];
    string[err],": ",text}[wanted;path]'[lines idx;errs]};
.kafka.i.decoded_rows:{[] $[`tmp in key `.kafka.i; .kafka.i.tmp; ()]};
.kafka.i.oracle:{[target;meta_cols]
  existing:$[-11h=type target; @[value;target;{[err] ::}]; ::];
  $[(type existing) in 98 99h; .kafka.i.tmp:0#((cols existing) except meta_cols)#0!existing; ![`.kafka.i;();0b;enlist `tmp]];
  };

/ the table coordinate `:pq:kafka:alias:topic/ - every door reads to the high-water mark at the call
.kafka.i.load:{[token;names] $[names~(::); .kafka.i.topics token; (),names]};
.kafka.i.topics:{[token] names:exec distinct topic from .kafka.i.metadata token; names where not names like "__*"};
.kafka.i.sample:{[token;topic]
  h:.kafka.i.handle token;
  if[(h;topic) in key .kafka.i.samples; :.kafka.i.samples (h;topic)];
  / a schema, not data: the sample skips a message that does not decode
  rows:.kafka.i.decoded[token;topic;.kafka.i.read[token;topic;::;neg .kafka.SAMPLE;::;.kafka.TIMEOUT];(enlist `ignore_errors)!enlist 1b];
  / an empty topic's columns are not its schema yet: sample it again next time
  if[count rows; .kafka.i.samples[(h;topic)]:rows];
  rows};
/ a read's batch decoded as the topic's subscription decodes it, the read's own options over that, and strict whatever
/ levers the subscription keeps
.kafka.i.decoded:{[token;topic;batch;given]
  h:.kafka.i.handle token;
  if[0=count batch; :$[(h;topic) in key .kafka.i.samples; 0#.kafka.i.samples (h;topic); .kafka.i.META#batch]];
  sub:.kafka.i.subs (h;topic);
  subscribed:not null sub`messages;
  opts:$[subscribed; .kafka.i.opts (h;topic); .kafka.i.OPTS];
  opts[`ignore_errors`store_rejects`rejects_table`tombstones]:(0b;0b;`reject_errors;`row);
  opts:opts,given;
  if[`rejects_table in key given; opts[`store_rejects]:1b];
  opts[`meta]:.kafka.i.META;
  dec:$[`decode in key given; given`decode; not subscribed; `json; sub[`decode]=`batch; `bytes; sub`decode];
  result:.kafka.i.decode[batch;dec;opts;$[subscribed; .kafka.i.targets (h;topic); ::]];
  bad:result 1;
  if[count bad;
    if[not any opts`ignore_errors`store_rejects; .kafka.i.strict[topic;first bad]];
    if[opts`store_rejects; .kafka.i.keep[opts`rejects_table;bad]]];
  $[98h=type result 0; result 0; 0#.kafka.i.META#batch]};
.kafka.i.strict:{[topic;bad]
  name:string topic;
  '"kafka: ",name," partition ",string[bad`partition]," offset ",string[bad`offset]," does not decode (",bad[`error],
    "); .kafka.read[h;(`",name,";(enlist `ignore_errors)!enlist 1b)] skips bad messages, store_rejects keeps them"};
/ rejects land keyed by topic partition offset, so a message rejected twice is one row
.kafka.i.keep:{[name;rejects]
  $[(type @[value;name;{[err] ::}]) in 98 99h; name upsert rejects; name set `topic`partition`offset xkey rejects]};
/ the .kafka.read bounds a where clause pushes down, from its literal partition=, partition in, offset within/>/>=/</<=
/ and msgtime within/>/>= constraints; the clause is re-applied in full afterwards, so a bound only prunes
.kafka.i.bounds:{[clauses]
  parts:(::); span:0N 0N; since:0Np; idx:0;
  clauses:(({[clause] (0h=type clause) and 3=count clause} each clauses)?0b)#clauses;
  while[idx<count clauses;
    clause:clauses idx; idx+:1;
    verb:clause 0; col:clause 1; lit:clause 2;
    if[col~`partition;
      if[((verb~(=)) and -7h=type lit) or (verb~(in)) and 7h=type lit; parts:lit]];
    if[(col~`offset) and -7h=type lit;
      if[verb~(>); span[0]:1+lit];
      if[verb~(>=); span[0]:lit];
      if[verb~(<); span[1]:lit-1];
      if[verb~(<=); span[1]:lit]];
    if[(col~`offset) and (verb~(within)) and 7h=type lit; span:lit];
    if[col~`msgtime;
      if[((verb~(>)) or verb~(>=)) and -12h=type lit; since:lit];
      if[(verb~(within)) and 12h=type lit; since:lit 0]]];
  given:(not parts~(::); not all null span; not null since);
  (`partition`offset`msgtime where given)!(parts;span;since) where given};
/ @ignore
.kafka.bind:{[token;topic] cols .kafka.i.sample[token;topic]};
/ @ignore
.kafka.meta:{[token;topic] meta .kafka.i.sample[token;topic]};
/ @ignore
/ the watermarks' span would count bad messages, tombstones and compaction gaps: no answer beats a wrong one
.kafka.count:{[token;topic] 0N};
/ @ignore
.kafka.get:{[token;topic] .kafka.read[token;topic]};
/ @ignore
.kafka.qsql:{[token;columns;tree]
  resolved:@[.pq.i.resolveTree[columns];tree;{[err] ::}];
  if[resolved~(::); :(::)];
  rows:.kafka.read[token;(resolved 0;.kafka.i.bounds resolved 1)];
  (?) . (enlist rows),1_resolved};
/ @ignore
.kafka.set:{[token;topic;rows] '`domain};
/ rows as JSON lines for the wire: full precision whatever \P, and finite timestamps, datetimes and dates as ISO 8601
.kafka.i.json_rows:{[rows]
  finite:{[at] not (null at) or $[-15h=type at; 0w=abs "f"$at; -14h=type at; 0Wi=abs "i"$at; 0W=abs "j"$at]};
  iso:{[finite;col] $[(type col) in 12 14 15h; {[finite;at] $[finite at; @[string at;$[-14h=type at; 4 7; 4 7 10];:;$[-14h=type at; "--"; "--T"]]; at]}[finite] each col; col]}[finite];
  {[row] .j.jd (row;(enlist `prec)!enlist 0)} each flip iso each flip rows};
/ @ignore
.kafka.upsert:{[token;topic;rows]
  if[not 98h=type rows; '`type];
  .kafka.i.produce[token;topic;::;.kafka.i.json_rows rows];
  .kafka.i.flush[token;.kafka.TIMEOUT];
  topic};
