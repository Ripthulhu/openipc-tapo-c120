# isp_avelum is in vendor SDK units, not a universal 8-bit pixel range.
s/\(lo:[[:space:]]*0,[[:space:]]*hi:\)[[:space:]]*255/\1null/g
s/\(['"]\) \/ 255\1/\1 raw\1/g
s/bands:[[:space:]]*\[{[[:space:]]*from:[[:space:]]*0,[[:space:]]*to:[[:space:]]*20,[[:space:]]*color:[[:space:]]*['"]rgba(255,193,7,\.10)['"][[:space:]]*}\]/bands:[]/g
