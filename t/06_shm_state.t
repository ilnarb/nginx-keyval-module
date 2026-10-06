use Test::Nginx::Socket 'no_plan';

no_root_location();
no_shuffle();

run_tests();

__DATA__

=== set and get with state file
--- http_config
keyval_zone zone=test:1M state=/tmp/nginx_keyval_test_state.bin;
keyval $cookie_data_key $keyval_data zone=test;
--- config
location = /get {
  return 200 "$request_uri($cookie_data_key): $keyval_data\n";
}
location = /set {
  set $keyval_data "test";
  return 200 "$request_uri($cookie_data_key): $keyval_data\n";
}
--- request eval
[
  "GET /get",
  "GET /set",
  "GET /get"
]
--- more_headers eval
[
  "Cookie: data_key=key",
  "Cookie: data_key=key",
  "Cookie: data_key=key"
]
--- response_body eval
[
  "/get(key): \n",
  "/set(key): test\n",
  "/get(key): test\n"
]

=== state file with ttl
--- http_config
keyval_zone zone=ttl:1M ttl=1h state=/tmp/nginx_keyval_test_state_ttl.bin;
keyval $cookie_data_key $keyval_data zone=ttl;
--- config
location = /get {
  return 200 "$request_uri($cookie_data_key): $keyval_data\n";
}
location = /set {
  set $keyval_data "test1h";
  return 200 "$request_uri($cookie_data_key): $keyval_data\n";
}
--- request eval
[
  "GET /get",
  "GET /set",
  "GET /get"
]
--- more_headers eval
[
  "Cookie: data_key=key",
  "Cookie: data_key=key",
  "Cookie: data_key=key"
]
--- response_body eval
[
  "/get(key): \n",
  "/set(key): test1h\n",
  "/get(key): test1h\n"
]

=== state file with timeout (legacy alias)
--- http_config
keyval_zone zone=timeout:1M timeout=1h state=/tmp/nginx_keyval_test_state_timeout.bin;
keyval $cookie_data_key $keyval_data zone=timeout;
--- config
location = /get {
  return 200 "$request_uri($cookie_data_key): $keyval_data\n";
}
--- request
GET /get
--- more_headers
Cookie: data_key=key
--- response_body
/get(key): 

=== state file with relative path
--- http_config
keyval_zone zone=rel:1M state=nginx_keyval_test_state_rel.bin;
keyval $cookie_data_key $keyval_data zone=rel;
--- config
location = /get {
  return 200 "$request_uri($cookie_data_key): $keyval_data\n";
}
location = /set {
  set $keyval_data "testrel";
  return 200 "$request_uri($cookie_data_key): $keyval_data\n";
}
--- request eval
[
  "GET /get",
  "GET /set",
  "GET /get"
]
--- more_headers eval
[
  "Cookie: data_key=key",
  "Cookie: data_key=key",
  "Cookie: data_key=key"
]
--- response_body eval
[
  "/get(key): \n",
  "/set(key): testrel\n",
  "/get(key): testrel\n"
]

=== conf invalid state file path (empty)
--- http_config
keyval_zone zone=test:1M state=;
keyval $cookie_data_key $keyval_data zone=test;
--- config
--- must_die

=== conf duplicate state file
--- http_config
keyval_zone zone=test:1M state=/tmp/nginx_keyval_test_state1.bin state=/tmp/nginx_keyval_test_state2.bin;
keyval $cookie_data_key $keyval_data zone=test;
--- config
--- must_die

=== conf duplicate state and ttl
--- http_config
keyval_zone zone=test:1M ttl=1h state=/tmp/nginx_keyval_test_state.bin state=/tmp/nginx_keyval_test_state2.bin;
keyval $cookie_data_key $keyval_data zone=test;
--- config
--- must_die
