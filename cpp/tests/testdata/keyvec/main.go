// Prints test vectors for package keys: each line is an encoding kind, the
// input, and the encoded key in hex. A string input is x and its bytes in
// hex; succ and next apply to the latest string's key.
package main

import (
	"encoding/hex"
	"fmt"
	"math"

	"cell-tnx/keys"
)

func main() {
	out := func(kind, in string, k []byte) { fmt.Printf("%s %s %s\n", kind, in, hex.EncodeToString(k)) }
	for _, x := range []uint64{0, 1, 255, 256, 1 << 32, math.MaxInt64, math.MaxUint64} {
		out("u64", fmt.Sprint(x), []byte(keys.U64(x)))
		out("uint", fmt.Sprint(x), keys.AppendUint(nil, x))
	}
	for _, x := range []int64{math.MinInt64, -256, -1, 0, 1, 255, math.MaxInt64} {
		out("int", fmt.Sprint(x), keys.AppendInt(nil, x))
	}
	for _, f := range []float64{math.Inf(-1), -math.MaxFloat64, -1.5, -math.SmallestNonzeroFloat64, math.Copysign(0, -1), 0,
		math.SmallestNonzeroFloat64, 1, 2.5, math.MaxFloat64, math.Inf(1), math.NaN()} {
		out("float", fmt.Sprintf("%016x", math.Float64bits(f)), keys.AppendFloat(nil, f))
	}
	for _, s := range []string{"", "a", "ab", "a\x00", "a\x00b", "\x00", "\x00\x00", "\xff", "a\xff\x00"} {
		k := keys.AppendString(nil, s)
		out("string", "x"+hex.EncodeToString([]byte(s)), k)
		out("succ", "-", []byte(keys.Succ(string(k))))
		out("next", "-", []byte(keys.Next(string(k))))
	}
	out("succ", "-", []byte(keys.Succ("\xff\xff")))
	out("null", "-", keys.AppendNull(nil))
}
