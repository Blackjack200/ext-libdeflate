--TEST--
Test that decompression works normally
--FILE--
<?php

$buffer = str_repeat("abcdefghijklmnopqrstuvwxyz", 1000);

foreach([
    ['libdeflate_deflate_compress','libdeflate_deflate_decompress'],
    ['libdeflate_zlib_compress','libdeflate_zlib_decompress'],
    ['libdeflate_gzip_compress','libdeflate_gzip_decompress'],
] as [$function, $defunc]){
    for($i = 1; $i <= 10; $i++){
        for($level = 0; $level <= 12; $level++){
            $compressed = $function($buffer, $level);

            if(isset($compressedBuffers[$level]) && $compressedBuffers[$level] !== $compressed){
                throw new \Error("Different output for compression on level $level on run $i");
            }

            $decompressed = $defunc($compressed);
            if($decompressed !== $buffer){
            throw new \Error("invalid func $function $defunc");
            }
        }
    }
}
echo "OK";
?>
--EXPECT--
OK
