--TEST--
Test that the value property is null for non-value tokens 
--SKIPIF--
<?php if (!extension_loaded("jsonreader")) print "skip"; ?>
--STDIN--
[{"foo":null}]
--FILE--
<?php
$rdr = new JSONReader();
$rdr->open('php://stdin');
do {
  var_dump($rdr->value);
} while ($rdr->read());
$rdr->close();
?>
--EXPECT--
NULL
NULL
NULL
string(3) "foo"
NULL
NULL
NULL

