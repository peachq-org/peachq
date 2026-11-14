/ Configure s3:// reads: credentials, region, endpoint and addressing for the objects read0, read1, hcount, -21!,
/ get and key read natively.  The settings are one dict keyed by the environment variable names AWS and KX use, and
/ once applied they are kept inside the process, never in a q variable.  The first s3:// read with nothing configured
/ applies .s3.config[::] itself, so a session whose environment already holds them needs no call at all.
/ .
/ @eg
/ .s3.config[::]
/ .s3.config `KX_S3_ENDPOINT`KX_S3_USE_PATH_REQUEST_STYLE!("http://127.0.0.1:9000";"1")
/ hcount `$":s3://bucket/db/2020.01.01/trade/sym"

.pq.load_natives`s3;

/ The settings the environment and the AWS shared files resolve to.  Later sources win: the config file's profile
/ section ([profile p], or [default]), then the credentials file's ([p]), then the environment.  The key id, secret and
/ session token are one set, taken whole from the last of those holding a key id, so two sources are never mixed.
/ AWS_REGION falls back to AWS_DEFAULT_REGION, then to the files.  The secret comes back as it is: this is an
/ explicit call.
/ @return (dict) a symbol key per setting found, AWS_ACCESS_KEY_ID AWS_SECRET_ACCESS_KEY AWS_SESSION_TOKEN AWS_REGION
/ AWS_PROFILE AWS_SHARED_CREDENTIALS_FILE AWS_CONFIG_FILE KX_S3_ENDPOINT KX_S3_USE_PATH_REQUEST_STYLE, each a string;
/ a setting that is not found is absent
/ @eg .s3.getAutoConfig[]
.s3.getAutoConfig:{[]
  ks:`AWS_ACCESS_KEY_ID`AWS_SECRET_ACCESS_KEY`AWS_SESSION_TOKEN`AWS_REGION`AWS_PROFILE,
    `AWS_SHARED_CREDENTIALS_FILE`AWS_CONFIG_FILE`KX_S3_ENDPOINT`KX_S3_USE_PATH_REQUEST_STYLE;
  found:{(where 0<count each x)#x};
  env:found[enlist[`AWS_REGION]!enlist getenv`AWS_DEFAULT_REGION],found ks!getenv each ks;
  home:$[count h:getenv`HOME; h; getenv`USERPROFILE];
  p:$[`AWS_PROFILE in key env; env`AWS_PROFILE; "default"];
  section:{[ks;f;s]
    l:l where 0<count each l:(trim each @[read0;hsym `$f;()]),enlist(),"[";
    l:l where not (first each l) in "#;";
    if[(count l)=i:l?"[",s,"]"; :(0#`)!()];
    b:(1+i)_l;
    kv:"=" vs/: b where "=" in/: b:((first each b)?"[")#b;
    d:(`$trim first each kv)!trim "=" sv/: 1_/:kv;
    m:`aws_access_key_id`aws_secret_access_key`aws_session_token`region!ks 0 1 2 3;
    (m k)!d k:key[d] inter key m}[ks];
  cred:found section[$[`AWS_SHARED_CREDENTIALS_FILE in key env; env`AWS_SHARED_CREDENTIALS_FILE; home,"/.aws/credentials"]; p];
  conf:found section[$[`AWS_CONFIG_FILE in key env; env`AWS_CONFIG_FILE; home,"/.aws/config"]; $[p~"default"; p; "profile ",p]];
  on:{[cs;a;b] $[`AWS_ACCESS_KEY_ID in key b; (cs _ a),b; a,cs _ b]}[3#ks];
  r:on[on[conf;cred];env];
  (ks inter key r)#r};

/ Replace the s3:// settings with cfg, or with .s3.getAutoConfig[] for ::.
/ @param cfg (dict) keys among those .s3.getAutoConfig answers, each value a string or a symbol; or ::
/ @return (dict) the settings applied, in .s3.getAutoConfig's key order, the secret and the session token shown as "***"
/ @throws option a key that is not a setting
/ @throws type cfg is not a dict or ::, or a value is not text
/ @throws access restricted mode (-u)
/ @eg .s3.config `AWS_REGION`AWS_ACCESS_KEY_ID`AWS_SECRET_ACCESS_KEY!("eu-west-1";"AKIA...";"...")
.s3.config:{[cfg]
  c:$[(::)~cfg; .s3.getAutoConfig[]; 99h<>type cfg; '`type; cfg];
  .s3.i.config key[c]!{$[-11h=type x; string x; 10h=abs type x; (),x; '`type]} each value c};
