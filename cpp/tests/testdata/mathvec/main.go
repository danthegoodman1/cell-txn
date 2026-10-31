// Prints math.Exp, math.Log and math.Pow results, as bits, for the C++
// GoPow check. On amd64, Exp takes an FMA path when the CPU has AVX and
// FMA, so generate the default file on such a CPU and the other with FMA
// disabled:
//
//	go run ./cpp/tests/testdata/mathvec > cpp/tests/testdata/math_vectors.txt
//	GODEBUG=cpu.fma=off go run ./cpp/tests/testdata/mathvec > cpp/tests/testdata/math_vectors_nofma.txt
package main

import (
	"fmt"
	"math"
	"math/rand/v2"
)

func main() {
	b := math.Float64bits
	r := rand.New(rand.NewPCG(8, 9))
	special := []float64{0, math.Copysign(0, -1), 1, -1, 0.5, 2, 3, 10, math.Inf(1), math.Inf(-1), math.NaN(),
		math.SmallestNonzeroFloat64, 2.2250738585072014e-308, math.MaxFloat64, 709.78, 709.7827128933841, 709.79,
		-708.4, -745.13, -745.2, -1e10, 1e-300, 0.7071067811865476, 0.7071067811865475, 1.4142135623730951}
	exps := append([]float64(nil), special...)
	logs := append([]float64(nil), special...)
	for range 300 {
		exps = append(exps, (r.Float64()*2-1)*750, r.Float64()-0.5, (r.Float64()*2-1)*5)
		logs = append(logs, r.Float64()*math.Pow(10, float64(r.IntN(80)-40)), r.Float64()*2,
			math.Float64frombits(r.Uint64N(1<<52)))
	}
	for _, x := range exps {
		fmt.Printf("exp %x %x\n", b(x), b(math.Exp(x)))
	}
	for _, x := range logs {
		fmt.Printf("log %x %x\n", b(x), b(math.Log(x)))
	}
	type pair struct{ x, y float64 }
	var pows []pair
	for _, x := range special {
		for _, y := range special {
			pows = append(pows, pair{x, y})
		}
	}
	// The Zipf generator's arguments.
	for range 300 {
		n := 2 + r.IntN(2000)
		theta := r.Float64() * 0.99
		u := r.Float64()
		eta := r.Float64()
		pows = append(pows, pair{float64(1 + r.IntN(n)), theta}, pair{2 / float64(n), 1 - theta},
			pair{0.5, theta}, pair{eta*u - eta + 1, 1 / (1 - theta)})
	}
	for range 300 {
		pows = append(pows, pair{r.Float64() * 100, (r.Float64()*2 - 1) * 50},
			pair{(r.Float64()*2 - 1) * 10, float64(r.IntN(41) - 20)},
			pair{r.Float64() * math.Pow(10, float64(r.IntN(60)-30)), (r.Float64()*2 - 1) * 20})
	}
	for _, p := range pows {
		fmt.Printf("pow %x %x %x\n", b(p.x), b(p.y), b(math.Pow(p.x, p.y)))
	}
}
