# fixsum.pl <rom.sfc> <header_file_offset_hex> — patch a correct SNES header
# checksum/complement. complement at hdr+0x1C, checksum at hdr+0x1E (little-endian).
# Standard algorithm: placeholders comp=FFFF sum=0000, sum all bytes, then
# checksum = sum & 0xFFFF, complement = checksum ^ 0xFFFF.
use strict; use warnings;
my ($file, $hoff) = @ARGV;
$hoff = hex($hoff);
open(my $fh, '+<:raw', $file) or die "open $file: $!";
my $d; { local $/; $d = <$fh>; }
substr($d, $hoff + 0x1C, 4) = chr(0xFF).chr(0xFF).chr(0x00).chr(0x00);
my $s = 0; $s += ord(substr($d, $_, 1)) for 0 .. length($d) - 1; $s &= 0xFFFF;
my $cc = $s ^ 0xFFFF;
substr($d, $hoff + 0x1C, 2) = chr($cc & 0xFF) . chr(($cc >> 8) & 0xFF);
substr($d, $hoff + 0x1E, 2) = chr($s  & 0xFF) . chr(($s  >> 8) & 0xFF);
seek($fh, 0, 0); print $fh $d; close($fh);
printf "%s: checksum=%04X complement=%04X\n", $file, $s, $cc;
