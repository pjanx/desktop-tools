#!/usr/bin/env perl
# Example slave command showing how to easily add additional things to the bar.
#
# You can place this in ~/.local/bin, customize it and direct wmstatus.conf
# towards by setting e.g.: command=~/.local/bin/wmstatus-weather.pl
use strict;
use warnings;
use Time::Piece;
use IO::Socket::INET;

my $host = 'www.yr.no';
my $path = '/place/Czech_Republic/Prague/Prague/forecast.xml';

# Retrieve current weather information from the Norwegian weather service
sub weather {
	# There are no redirects and it's not exactly confidential either
	my $sock = IO::Socket::INET->new(
		PeerAddr => $host,
		PeerPort => 'http(80)',
		Proto => 'tcp'
	) or return '?';

	print $sock "GET $path HTTP/1.1\r\n"
		. "Host: $host\r\n"
		. "Connection: close\r\n\r\n";

	# Quick and dirty XML parsing is more than fine for our purpose
	my ($offset, $acceptable, $temp, $symbol) = (0, 0);
	while (<$sock>) {
		$offset = $1 * 60 if /utcoffsetMinutes="(.+?)"/;
		next unless /<time/ .. /<\/time/;

		# It gives forecast, so it doesn't necessarily contain the present;
		# just pick the first thing that's no longer invalid
		if (/from="(.+?)" to="(.+?)"/) {
			$acceptable = Time::Piece->strptime($2, '%Y-%m-%dT%H:%M:%S')
				- $offset >= gmtime;
		}
		if ($acceptable) {
			$symbol = $1 if /<symbol .* name="(.+?)"/;
			$temp = "$2 °${\uc $1}"
				if /<temperature unit="(.).+?" value="(.+?)"/;
		}
		return "$temp ($symbol)" if $temp && $symbol;
	}
	return 'Weather error';
}

# We need to be careful not to overload the service so that they don't ban us
binmode STDOUT; $| = 1; while (1) { print weather() . "\n\n"; sleep 3600; }
