package workload

import (
	"fmt"
	"math"
	"math/rand/v2"

	"cell-tnx/check"
	"cell-tnx/keys"
	"cell-tnx/txn"
)

// Users is the synthetic workload: hot user rows picked by Zipf(Theta),
// with transaction types that touch disjoint or shared columns.
type Users struct {
	Rows  uint64
	Theta float64
	// Mix weights, indexed by the users* transaction types.
	Mix   [nusers]int
	Think float64
	zipf  *Zipf
}

const (
	usersPay     = iota // wallet -= x, as a delta
	usersDeposit        // wallet += x, as a delta
	usersProfile        // set email
	usersAudit          // read email, insert an audit row
	usersSame           // read wallet, set wallet
	usersRead           // read-only lookup of two users
	nusers
)

const (
	uName = iota
	uEmail
	uWallet
)

const (
	tUsers = iota
	tAudit
)

// NewUsers returns the workload with the given rows, skew and mix.
func NewUsers(rows uint64, theta float64, mix [nusers]int, think float64) *Users {
	return &Users{Rows: rows, Theta: theta, Mix: mix, Think: think, zipf: NewZipf(rows, theta)}
}

// NewUsersFrom draws a configuration from r.
func NewUsersFrom(r *rand.Rand) *Users {
	var mix [nusers]int
	for i := range mix {
		mix[i] = r.IntN(6)
	}
	mix[usersPay]++
	return NewUsers(uint64(2+r.IntN(30)), r.Float64()*0.99, mix, r.Float64()*0.3)
}

func (w *Users) Name() string { return "users" }

func (w *Users) String() string {
	return fmt.Sprintf("rows=%d theta=%.2f mix=%v think=%.2f", w.Rows, w.Theta, w.Mix, w.Think)
}

func (w *Users) Schemas() []txn.Schema {
	return []txn.Schema{
		{
			Name: "users",
			Cols: []string{"name", "email", "wallet"},
			Checks: []txn.Check{{
				Name: "wallet >= 0",
				Cols: txn.Col(uWallet),
				OK:   func(r []txn.Value) bool { x, _ := r[uWallet].(int64); return x >= 0 },
			}},
		},
		{Name: "audit", Cols: []string{"user", "email"}},
	}
}

func (w *Users) Load(c *Client) error {
	return c.Do(func(tx *check.Tx) error {
		for id := range w.Rows {
			if err := tx.Insert(tUsers, keys.U64(id), []txn.Value{c.Uniq(), c.Uniq(), c.Rand.Int64N(30)}); err != nil {
				return err
			}
		}
		return nil
	})
}

func (w *Users) pick(r *rand.Rand) int {
	total := 0
	for _, x := range w.Mix {
		total += x
	}
	n := r.IntN(total)
	for i, x := range w.Mix {
		if n < x {
			return i
		}
		n -= x
	}
	panic("unreachable")
}

func (w *Users) think(c *Client) {
	if c.Rand.Float64() < w.Think {
		c.Env.Sleep(1 + c.Rand.Int64N(50))
	}
}

func (w *Users) Step(c *Client) error {
	r := c.Rand
	n := w.zipf.Next(r)
	id := keys.U64(n)
	x := 1 + r.Int64N(5)
	kind := w.pick(r)
	return expect(c.Do(func(tx *check.Tx) error {
		switch kind {
		case usersPay:
			return tx.Add(tUsers, id, uWallet, -x)
		case usersDeposit:
			return tx.Add(tUsers, id, uWallet, x)
		case usersProfile:
			v := make([]txn.Value, 3)
			v[uEmail] = c.Uniq()
			return tx.Set(tUsers, id, txn.Col(uEmail), v)
		case usersAudit:
			u, ok, err := tx.Get(tUsers, id, txn.Col(uEmail))
			if err != nil || !ok {
				return fmt.Errorf("users: get %d: %v %v", n, ok, err)
			}
			w.think(c)
			return tx.Insert(tAudit, keys.U64(c.DB.NextID(tAudit)), []txn.Value{int64(n), u[uEmail]})
		case usersSame:
			u, ok, err := tx.Get(tUsers, id, txn.Col(uWallet))
			if err != nil || !ok {
				return fmt.Errorf("users: get %d: %v %v", n, ok, err)
			}
			w.think(c)
			v := make([]txn.Value, 3)
			v[uWallet] = u[uWallet].(int64) + 1
			return tx.Set(tUsers, id, txn.Col(uWallet), v)
		default:
			for _, pk := range []string{id, keys.U64(w.zipf.Next(r))} {
				if _, _, err := tx.Get(tUsers, pk, txn.Cols(uName, uEmail, uWallet)); err != nil {
					return err
				}
				w.think(c)
			}
			return nil
		}
	}))
}

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
