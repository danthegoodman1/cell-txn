package main

import (
	"math"
	"math/rand/v2"
)

// Zipf draws ranks in [0, n) with skew theta in [0, 1), following Gray et
// al., "Quickly Generating Billion-Record Synthetic Databases".
type Zipf struct {
	n                   uint64
	theta, alpha, zetan float64
	eta                 float64
}

func NewZipf(n uint64, theta float64) *Zipf {
	z := &Zipf{n: n, theta: theta}
	if theta == 0 {
		return z
	}
	zeta := func(k uint64) float64 {
		s := 0.0
		for i := uint64(1); i <= k; i++ {
			s += 1 / math.Pow(float64(i), theta)
		}
		return s
	}
	z.zetan = zeta(n)
	z.alpha = 1 / (1 - theta)
	z.eta = (1 - math.Pow(2/float64(n), 1-theta)) / (1 - zeta(2)/z.zetan)
	return z
}

func (z *Zipf) Next(r *rand.Rand) uint64 {
	if z.theta == 0 || z.n < 2 {
		return r.Uint64N(z.n)
	}
	u := r.Float64()
	uz := u * z.zetan
	if uz < 1 {
		return 0
	}
	if uz < 1+math.Pow(0.5, z.theta) {
		return 1
	}
	return min(z.n-1, uint64(float64(z.n)*math.Pow(z.eta*u-z.eta+1, z.alpha)))
}
