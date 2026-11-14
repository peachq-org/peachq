/ Configure s3:// reads natively: credentials, region, endpoint and addressing.  A config is a dict keyed by the AWS
/ and KX environment variable names, scoped by path prefix and kept inside the process, never in a q variable.  The
/ default (scope s3://) resolves as .s3.config[::;::] at the first s3:// read unless set.
/ @eg .s3.config["s3://flatfiles";`KX_S3_ENDPOINT`AWS_REGION!("https://files.massive.com";"us-east-1")]

.pq.load_natives`s3;

/ The settings the environment and the AWS shared files resolve to; later sources win: the config file's profile
/ section ([profile p] or [default]), the credentials file's ([p]), then the environment.  The key id, secret and token
/ come whole from the last source holding a key id.  AWS_REGION falls back to AWS_DEFAULT_REGION and KX_S3_ENDPOINT to
/ the files' endpoint_url.  The secret comes back as it is: this is an explicit call.
/ @return (dict) a string per setting found, keyed by .s3.config's names
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
    m:`aws_access_key_id`aws_secret_access_key`aws_session_token`region`endpoint_url!ks 0 1 2 3 7;
    (m k)!d k:key[d] inter key m}[ks];
  cred:found section[$[`AWS_SHARED_CREDENTIALS_FILE in key env; env`AWS_SHARED_CREDENTIALS_FILE; home,"/.aws/credentials"]; p];
  conf:found section[$[`AWS_CONFIG_FILE in key env; env`AWS_CONFIG_FILE; home,"/.aws/config"]; $[p~"default"; p; "profile ",p]];
  on:{[cs;a;b] $[`AWS_ACCESS_KEY_ID in key b; (cs _ a),b; a,cs _ b]}[3#ks];
  r:on[on[conf;cred];env];
  (ks inter key r)#r};

/ Set the config of the paths under scope.  A path takes the config of its longest scope that is a whole-component
/ prefix (s3://flat does not cover s3://flatfiles/x), else the default's, and never a setting from another config.
/ Each config is mirrored as a DuckDB secret of the same scope.
/ @param scope (string|symbol|list) path prefixes sharing the config; :: is the default, "s3://"
/ @param cfg (dict) text values keyed among AWS_ACCESS_KEY_ID AWS_SECRET_ACCESS_KEY AWS_SESSION_TOKEN AWS_REGION
/ AWS_PROFILE AWS_SHARED_CREDENTIALS_FILE AWS_CONFIG_FILE KX_S3_ENDPOINT KX_S3_USE_PATH_REQUEST_STYLE; :: for
/ .s3.getAutoConfig[]; () drops the scopes
/ @return (dict) the settings applied, the secret and the session token shown as "***"
/ @throws option a key that is not a setting
/ @throws type cfg is not a dict, :: or (), or a value is not text
/ @throws domain a scope that is not an s3://, s3a:// or s3n:// prefix
/ @eg .s3.config["s3://flatfiles";()]
.s3.config:{[scope;cfg]
  s:$[(::)~scope; enlist "s3://"; 10h=type scope; enlist scope; {$[10h=abs type x; (),x; -11h=type x; string x; '`type]} each (),scope];
  c:$[(::)~cfg; .s3.getAutoConfig[]; ()~cfg; (); 99h<>type cfg; '`type; cfg];
  .s3.i.config[s; $[99h=type c; key[c]!{$[-11h=type x; string x; 10h=abs type x; (),x; '`type]} each value c; c]]};

/ The config a path is read with, masked; the empty dict when nothing covers it.
/ @eg .s3.which `:s3://flatfiles/us_stocks_sip/x.csv.gz
.s3.which:{[path] .s3.i.which $[-11h=type path; string path; path]};

/ A row per scope: the scope, then each setting of its config ("" for none), the secret and the token as "***".
/ @eg .s3.configs[]
.s3.configs:{[] .s3.i.configs[]};
