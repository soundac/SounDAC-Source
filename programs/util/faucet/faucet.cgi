#!/usr/bin/perl -wT

use strict;
use LWP::UserAgent;
use JSON;
use CGI::Simple;

$main::FAUCET_URL = "http://127.0.0.1:18091/";

$CGI::Simple::POST_MAX = 1024;

$main::Q = CGI::Simple->new;

if (!$main::Q->param('name')) {
    &error("Missing name!");
} elsif (!$main::Q->param('owner')) {
    &error("Missing owner key!");
} elsif (!$main::Q->param('active')) {
    &error("Missing active key!");
} elsif (!$main::Q->param('basic')) {
    &error("Missing basic key!");
} elsif (!$main::Q->param('memo')) {
    &error("Missing memo key!");
}

my $req = HTTP::Request->new("POST", $main::FAUCET_URL);
$req->header("Content-Type" => "application/json");
my $json = JSON->new;
$req->content($json->encode({id => 0,
			     method => "create_account_with_keys",
			     params => ["faucet",
				        $main::Q->param('name'),
					"",
				        $main::Q->param('owner'),
				        $main::Q->param('active'),
				        $main::Q->param('basic'),
				        $main::Q->param('memo'),
				        1]}));
my $ua = LWP::UserAgent->new;
my $res = $ua->simple_request($req);
my $data = $json->decode($res->content);

if ($data->{error}) {
    &error(ref($data->{error}) eq "SCALAR" ? $data->{error} : $json->pretty->encode($data->{error}));
}

my $msg = $json->pretty->encode($data);
print $main::Q->header( -type => 'text/plain; charset=utf-8',
			-status => '200 OK',
			-content_length => length($msg));
print $msg;

exit 0;

sub error {
my $msg = shift;

    print $main::Q->header(-type => 'text/plain; charset=utf-8',
			   -status => '400 Bad Request',
			   -content_length => length($msg));
    print $msg;
    exit 1
}
