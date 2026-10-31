// Prints each workload drawn from many seeds, as the simulator prints it
// with %+v, and the generator's next value, which proves the draw count;
// then Zipf ranks drawn from each Users configuration and from the bench's
// default skew:
//
//	go run ./cpp/tests/testdata/wlvec > cpp/tests/testdata/workload_vectors.txt
package main

import (
	"fmt"
	"math"
	"math/rand/v2"

	"cell-tnx/workload"
)

func main() {
	draws := []struct {
		name string
		draw func(*rand.Rand) workload.Workload
	}{
		{"random", func(r *rand.Rand) workload.Workload { return workload.NewRandom(r) }},
		{"users", func(r *rand.Rand) workload.Workload { return workload.NewUsersFrom(r) }},
		{"tpcc", func(r *rand.Rand) workload.Workload { return workload.NewTPCCFrom(r) }},
	}
	for seed := range uint64(1000) {
		for _, d := range draws {
			r := rand.New(rand.NewPCG(seed, 7))
			w := d.draw(r)
			fmt.Printf("%s %d %d %+v\n", d.name, seed, r.Uint64(), w)
			if u, ok := w.(*workload.Users); ok {
				z := workload.NewZipf(u.Rows, u.Theta)
				fmt.Printf("zipf %d %x", u.Rows, math.Float64bits(u.Theta))
				for range 20 {
					fmt.Printf(" %d", z.Next(r))
				}
				fmt.Println()
			}
		}
	}
	r := rand.New(rand.NewPCG(1, 1))
	for _, n := range []uint64{1, 2, 3, 1000, 100000} {
		for _, theta := range []float64{0, 0.5, 0.9, 0.99} {
			z := workload.NewZipf(n, theta)
			fmt.Printf("zipf %d %x", n, math.Float64bits(theta))
			for range 50 {
				fmt.Printf(" %d", z.Next(r))
			}
			fmt.Println()
		}
	}
}
