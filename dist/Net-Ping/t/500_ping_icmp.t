# Test to perform icmp protocol testing.
# Root access is required.

use strict;
use Config;

BEGIN {
  unless (eval "require Socket") {
    print "1..0 \# Skip: no Socket\n";
    exit;
  }
  unless ($Config{d_getpbyname}) {
    print "1..0 \# Skip: no getprotobyname\n";
    exit;
  }
}

use Test::More tests => 2;
BEGIN {use_ok('Net::Ping')};

SKIP: {
  skip "icmp ping requires root privileges.", 1
    if !Net::Ping::_isroot() or $^O eq 'MSWin32';
  my $p = new Net::Ping "icmp";
  is($p->ping("127.0.0.1"), 1, "icmp ping 127.0.0.1");
}

