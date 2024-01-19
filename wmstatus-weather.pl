#!/usr/bin/env perl
# Example slave command showing how to easily add additional things to the bar.
#
# You can place this in ~/.local/bin, customize it and direct wmstatus.conf
# towards by setting e.g.: command=~/.local/bin/wmstatus-weather.pl
use strict;
use warnings;
use Time::Piece;
use File::Basename;

# Retrieve current weather information from the Norwegian weather service,
# see https://api.met.no/doc/ for its documentation
my $base = 'https://api.met.no/weatherapi';
my $agent = basename($0) =~ s/[^-!#$%&'*+.^_`|~[:alnum:]]//gr;

# https://www.yr.no/storage/lookup/English.csv.zip
my $where = 'lat=50.08804&lon=14.42076&altitude=202';  # Prague
my %legends;

sub retrieve_legends {
	# HTTP/Tiny supports TLS, but with non-core IO::Socket::SSL, so use cURL
	open(my $sock, '-|', 'curl', '-sSA', $agent,
		'https://raw.githubusercontent.com/' .
		'metno/weathericons/main/weather/legend.csv') or return $!;
	while (local $_ = <$sock>) { $legends{$1} = $2 if /^(.+?),(.+?),/ }
	close($sock);
}

sub weather {
	# We might want to rewrite this to use the JSON API (/compact),
	# see https://developer.yr.no/doc/guides/getting-started-from-forecast-xml
	open(my $sock, '-|', 'curl', '-sA', $agent,
		"$base/locationforecast/2.0/classic?$where") or return $!;

	# Quick and dirty XML parsing is more than fine for our purpose
	my ($acceptable, $temp, $symbol) = (0, undef, undef);
	while (<$sock>) {
		next unless m|<time| .. m|</time|;

		# It gives forecast, so it doesn't necessarily contain the present;
		# just process the earliest entries that aren't yet invalid
		$acceptable = Time::Piece->strptime($2, '%Y-%m-%dT%H:%M:%SZ') >= gmtime
			if /from="(.+?)" to="(.+?)"/;
		next unless $acceptable;

		# Temperature comes from a zero-length time interval, separately
		$symbol = $1 if /<symbol.*? code="([^_"]+)/;
		$temp = "$2 °" . uc $1 if /<temperature.*? unit="(.).+?" value="(.+?)"/;
		if ($temp && $symbol) {
			retrieve_legends if !%legends;

			close($sock);
			return "$temp (" . ($legends{$symbol} || $symbol) . ")";
		}
	}
	close($sock);
	return "No weather ($?)";
}

# Be careful not to overload the service so that they don't ban us
binmode STDOUT; $| = 1; while (1) { print weather() . "\n\n"; sleep 3600; }
