// Prints test vectors for math/rand/v2's PCG and Rand methods.
package main

import (
	"fmt"
	"math/rand/v2"
)

func main() {
	for _, s := range [][2]uint64{{0, 0}, {1, 2}, {42, 0x9e3779b97f4a7c15}, {^uint64(0), 7}} {
		r := rand.New(rand.NewPCG(s[0], s[1]))
		fmt.Printf("seed %d %d\n", s[0], s[1])
		for i := 0; i < 4; i++ {
			fmt.Printf("u %d\n", r.Uint64())
		}
		for _, n := range []int{1, 2, 3, 7, 10, 1000, 1 << 20, 1<<62 + 3} {
			fmt.Printf("i %d %d\n", n, r.IntN(n))
		}
		for _, n := range []uint64{5, 1 << 40, ^uint64(0) - 2} {
			fmt.Printf("n %d %d\n", n, r.Uint64N(n))
		}
		for _, n := range []int64{3, 1 << 33} {
			fmt.Printf("l %d %d\n", n, r.Int64N(n))
		}
		for i := 0; i < 3; i++ {
			fmt.Printf("f %.17g\n", r.Float64())
		}
	}
}
