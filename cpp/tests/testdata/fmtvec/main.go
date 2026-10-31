// Prints floats under %v and %g for the C++ GoFloat check.
package main

import (
	"fmt"
	"math"
	"math/rand/v2"
)

func main() {
	fs := []float64{0, math.Copysign(0, -1), 1, -1, 0.5, 0.002, 1e-05, 0.0001, 0.00001, 100000, 1e6, 1234567, 123456789,
		1e21, 1e-300, 0.032192702151443896, 2.5e-7, 99999.99, 999999.5, math.MaxFloat64, math.SmallestNonzeroFloat64, 3.14}
	r := rand.New(rand.NewPCG(5, 6))
	for i := 0; i < 2000; i++ {
		fs = append(fs, r.Float64(), r.Float64()*math.Pow(10, float64(r.IntN(40)-20)), -r.NormFloat64()*1e5)
	}
	for _, f := range fs {
		fmt.Printf("%x %v %g\n", math.Float64bits(f), f, f)
	}
}
