/ Configure s3:// reads: credentials, region, endpoint and addressing for the objects read0, read1, hcount, -21!,
/ get and key read natively.  The settings are one dict keyed by the environment variable names AWS and KX use, and
/ they are kept inside the process, never in a q variable.  The first s3:// read with nothing configured applies
/ .s3.getAutoConfig[] itself, so a session whose environment already holds them needs no call at all.
/ .
/ @eg
/ .s3.config[::]
/ .s3.config `KX_S3_ENDPOINT`KX_S3_USE_PATH_REQUEST_STYLE!("http://127.0.0.1:9000";"1")
/ hcount `$":s3://bucket/db/2020.01.01/trade/sym"

.pq.load_natives`s3;

/ The settings the environment and the AWS shared files resolve to: the environment first, then the profile's section
/ of the credentials file, then of the config file.  The secret comes back as it is: this is an explicit call.
/ @return (dict) a symbol key per setting found, AWS_ACCESS_KEY_ID AWS_SECRET_ACCESS_KEY AWS_SESSION_TOKEN AWS_REGION
/ (AWS_DEFAULT_REGION when unset) AWS_PROFILE AWS_SHARED_CREDENTIALS_FILE AWS_CONFIG_FILE KX_S3_ENDPOINT
/ KX_S3_USE_PATH_REQUEST_STYLE, each a string; a setting that is not found is absent
/ @throws access restricted mode (-u)
/ @eg .s3.getAutoConfig[]
.s3.getAutoConfig:{[] .s3.i.auto[]};

/ Replace the s3:// settings with cfg, or with .s3.getAutoConfig[] for ::.
/ @param cfg (dict) keys among those .s3.getAutoConfig answers, each value a string or a symbol; or ::
/ @return (dict) the settings applied, in .s3.getAutoConfig's key order, the secret and the session token shown as "***"
/ @throws option a key that is not a setting
/ @throws type cfg is not a dict or ::, or a value is not text
/ @throws access restricted mode (-u)
/ @eg .s3.config `AWS_REGION`AWS_ACCESS_KEY_ID`AWS_SECRET_ACCESS_KEY!("eu-west-1";"AKIA...";"...")
.s3.config:{[cfg]
  .s3.i.config $[(::)~cfg; cfg; 99h<>type cfg; '`type; key[cfg]!{$[-11h=type x; string x; 10h=abs type x; (),x; '`type]} each value cfg]};
