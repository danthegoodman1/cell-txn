package workload

import (
	"errors"
	"fmt"
	"math/rand/v2"

	"cell-tnx/check"
	"cell-tnx/keys"
	"cell-tnx/txn"
)

// TPCC runs TPC-C NewOrder and Payment, scaled down, plus a read-only
// audit that verifies consistency conditions 1–4. Payment's YTD, balance
// and count updates and NewOrder's stock counters are Adds, so the mode
// alone decides whether they commute. Like TPC-C, 60% of Payments find
// the customer by last name through a secondary index.
type TPCC struct {
	Warehouses uint64
	Districts  uint64 // per warehouse, at most 255
	Customers  uint64 // per district
	Items      uint64
	MaxLines   int // order lines per NewOrder, at most 15
	// Mix weights for NewOrder, Payment and the audit.
	Mix   [3]int
	Think float64
}

// Tables and columns.
const (
	tWarehouse = iota
	tDistrict
	tCustomer
	tHistory
	tOrders
	tNewOrder
	tOrderLine
	tItem
	tStock
)

const (
	wTax = iota
	wYTD
	wName
)

const (
	dTax = iota
	dYTD
	dNextOID
	dName
)

const (
	cDiscount = iota
	cCredit
	cLast
	cBalance
	cYTDPayment
	cPaymentCnt
	cWID
	cDID
)

const (
	oCID = iota
	oOLCnt
	oAllLocal
)

const (
	olIID = iota
	olSupplyWID
	olQuantity
	olAmount
)

const (
	sQuantity = iota
	sYTD
	sOrderCnt
	sRemoteCnt
)

// Keys pack composite primary keys into 8 bytes so key order matches
// TPC-C's.
func wKey(w uint64) string            { return keys.U64(w) }
func dKey(w, d uint64) string         { return keys.U64(w<<8 | d) }
func cKey(w, d, c uint64) string      { return keys.U64(w<<40 | d<<32 | c) }
func oKey(w, d, o uint64) string      { return keys.U64(w<<40 | d<<32 | o) }
func olKey(w, d, o, ol uint64) string { return keys.U64(w<<44 | d<<36 | o<<4 | ol) }
func sKey(w, i uint64) string         { return keys.U64(w<<32 | i) }

// lastKey encodes the customer index key (warehouse, district, last name).
func lastKey(w, d, last int64) string {
	return string(keys.AppendInt(keys.AppendInt(keys.AppendInt(nil, w), d), last))
}

// NewTPCC returns the standard 45:43 NewOrder:Payment mix with a small
// share of audits.
func NewTPCC(warehouses, districts, customers, items uint64, maxLines int) *TPCC {
	return &TPCC{Warehouses: warehouses, Districts: districts, Customers: customers, Items: items, MaxLines: maxLines, Mix: [3]int{45, 43, 4}}
}

// NewTPCCFrom draws a small, contended configuration from r.
func NewTPCCFrom(r *rand.Rand) *TPCC {
	w := NewTPCC(uint64(1+r.IntN(2)), uint64(1+r.IntN(4)), uint64(2+r.IntN(10)), uint64(5+r.IntN(40)), 1+r.IntN(6))
	w.Mix[2] = 1 + r.IntN(10)
	w.Think = r.Float64() * 0.2
	return w
}

func (w *TPCC) Name() string { return "tpcc" }

func (w *TPCC) Schemas() []txn.Schema {
	byLast := txn.Index{
		Name: "c_last",
		Cols: txn.Cols(cWID, cDID, cLast),
		Key: func(r []txn.Value) (string, bool) {
			return lastKey(asInt(r[cWID]), asInt(r[cDID]), asInt(r[cLast])), false
		},
		BucketPrefix: 27,
	}
	return []txn.Schema{
		{Name: "warehouse", Cols: []string{"w_tax", "w_ytd", "w_name"}},
		{Name: "district", Cols: []string{"d_tax", "d_ytd", "d_next_o_id", "d_name"}},
		{Name: "customer", Cols: []string{"c_discount", "c_credit", "c_last", "c_balance", "c_ytd_payment", "c_payment_cnt", "c_w_id", "c_d_id"}, Indexes: []txn.Index{byLast}},
		{Name: "history", Cols: []string{"h_c_id", "h_d_id", "h_w_id", "h_amount"}},
		{Name: "orders", Cols: []string{"o_c_id", "o_ol_cnt", "o_all_local"}},
		{Name: "new_order", Cols: []string{"no_flag"}},
		{Name: "order_line", Cols: []string{"ol_i_id", "ol_supply_w_id", "ol_quantity", "ol_amount"}},
		{Name: "item", Cols: []string{"i_price"}},
		{Name: "stock", Cols: []string{"s_quantity", "s_ytd", "s_order_cnt", "s_remote_cnt"}},
	}
}

// lastNames is how many distinct last names each district's customers
// share, so a by-name lookup usually finds several customers.
func (w *TPCC) lastNames() uint64 { return max(1, w.Customers/3) }

// Load inserts the initial data in transactions of at most 1000 rows.
// Consistency condition 1 holds after every one of them.
func (w *TPCC) Load(c *Client) error {
	r := c.Rand
	var rows []func(tx *check.Tx) error
	ins := func(tbl int, pk string, row ...int64) {
		rows = append(rows, func(tx *check.Tx) error { return tx.Insert(tbl, pk, values(row...)) })
	}
	for i := uint64(1); i <= w.Items; i++ {
		ins(tItem, keys.U64(i), 1+r.Int64N(100))
	}
	for wh := uint64(1); wh <= w.Warehouses; wh++ {
		for i := uint64(1); i <= w.Items; i++ {
			ins(tStock, sKey(wh, i), 10+r.Int64N(91), 0, 0, 0)
		}
		for d := uint64(1); d <= w.Districts; d++ {
			for cu := uint64(1); cu <= w.Customers; cu++ {
				last := int64(cu % w.lastNames())
				ins(tCustomer, cKey(wh, d, cu), r.Int64N(5000), r.Int64N(10)/9, last, -10, 10, 1, int64(wh), int64(d))
			}
		}
	}
	// Each warehouse and its districts go in one transaction, after the
	// rows they refer to.
	for wh := uint64(1); wh <= w.Warehouses; wh++ {
		var group []func(tx *check.Tx) error
		group = append(group, func(tx *check.Tx) error {
			return tx.Insert(tWarehouse, wKey(wh), values(r.Int64N(2000), 30000*int64(w.Districts), c.Uniq()))
		})
		for d := uint64(1); d <= w.Districts; d++ {
			group = append(group, func(tx *check.Tx) error {
				return tx.Insert(tDistrict, dKey(wh, d), values(r.Int64N(2000), 30000, 1, c.Uniq()))
			})
		}
		rows = append(rows, func(tx *check.Tx) error {
			for _, f := range group {
				if err := f(tx); err != nil {
					return err
				}
			}
			return nil
		})
	}
	for len(rows) > 0 {
		n := min(len(rows), 1000)
		if err := c.Do(func(tx *check.Tx) error {
			for _, f := range rows[:n] {
				if err := f(tx); err != nil {
					return err
				}
			}
			return nil
		}); err != nil {
			return err
		}
		rows = rows[n:]
	}
	return nil
}

func (w *TPCC) Step(c *Client) error {
	total := w.Mix[0] + w.Mix[1] + w.Mix[2]
	switch n := c.Rand.IntN(total); {
	case n < w.Mix[0]:
		return w.newOrder(c)
	case n < w.Mix[0]+w.Mix[1]:
		return w.payment(c)
	default:
		return w.audit(c)
	}
}

func (w *TPCC) think(c *Client) {
	if c.Rand.Float64() < w.Think {
		c.Env.Sleep(1 + c.Rand.Int64N(50))
	}
}

func (w *TPCC) otherWarehouse(r *rand.Rand, wh uint64) uint64 {
	o := 1 + r.Uint64N(w.Warehouses-1)
	if o >= wh {
		o++
	}
	return o
}

func asInt(v txn.Value) int64 {
	x, _ := v.(int64)
	return x
}

func values(xs ...int64) []txn.Value {
	out := make([]txn.Value, len(xs))
	for i, x := range xs {
		out[i] = x
	}
	return out
}

// get reads a row that must exist, as int64s.
func get(tx *check.Tx, tbl int, pk string, cols txn.ColMask) ([]int64, error) {
	row, ok, err := tx.Get(tbl, pk, cols)
	if err == nil && !ok {
		err = fmt.Errorf("tpcc: table %d row %x missing", tbl, pk)
	}
	out := make([]int64, len(row))
	for i, v := range row {
		out[i] = asInt(v)
	}
	return out, err
}

func (w *TPCC) newOrder(c *Client) error {
	r := c.Rand
	wh, d, cu := 1+r.Uint64N(w.Warehouses), 1+r.Uint64N(w.Districts), 1+r.Uint64N(w.Customers)
	type line struct {
		item, supply uint64
		qty          int64
	}
	lines := make([]line, 1+r.IntN(w.MaxLines))
	allLocal := int64(1)
	for i := range lines {
		l := line{item: 1 + r.Uint64N(w.Items), supply: wh, qty: 1 + r.Int64N(10)}
		if w.Warehouses > 1 && r.IntN(100) == 0 {
			l.supply = w.otherWarehouse(r, wh)
			allLocal = 0
		}
		lines[i] = l
	}
	if r.IntN(100) == 0 {
		lines[len(lines)-1].item = w.Items + 1 // unused item: roll back
	}
	return expect(c.Do(func(tx *check.Tx) error {
		if _, err := get(tx, tWarehouse, wKey(wh), txn.Col(wTax)); err != nil {
			return err
		}
		dist, err := get(tx, tDistrict, dKey(wh, d), txn.Cols(dTax, dNextOID))
		if err != nil {
			return err
		}
		o := uint64(dist[dNextOID])
		dist[dNextOID]++
		if err := tx.Set(tDistrict, dKey(wh, d), txn.Col(dNextOID), values(dist...)); err != nil {
			return err
		}
		if _, err := get(tx, tCustomer, cKey(wh, d, cu), txn.Cols(cDiscount, cCredit, cLast)); err != nil {
			return err
		}
		w.think(c)
		if err := tx.Insert(tOrders, oKey(wh, d, o), values(int64(cu), int64(len(lines)), allLocal)); err != nil {
			return err
		}
		if err := tx.Insert(tNewOrder, oKey(wh, d, o), values(0)); err != nil {
			return err
		}
		for n, l := range lines {
			item, ok, err := tx.Get(tItem, keys.U64(l.item), txn.Col(0))
			if err != nil {
				return err
			}
			if !ok {
				return errAbort
			}
			sk := sKey(l.supply, l.item)
			s, err := get(tx, tStock, sk, txn.Col(sQuantity))
			if err != nil {
				return err
			}
			if s[sQuantity] -= l.qty; s[sQuantity] < 10 {
				s[sQuantity] += 91
			}
			err = errors.Join(
				tx.Set(tStock, sk, txn.Col(sQuantity), values(s...)),
				tx.Add(tStock, sk, sYTD, l.qty),
				tx.Add(tStock, sk, sOrderCnt, 1),
			)
			if err == nil && l.supply != wh {
				err = tx.Add(tStock, sk, sRemoteCnt, 1)
			}
			if err == nil {
				err = tx.Insert(tOrderLine, olKey(wh, d, o, uint64(n+1)), values(int64(l.item), int64(l.supply), l.qty, l.qty*asInt(item[0])))
			}
			if err != nil {
				return err
			}
		}
		return nil
	}))
}

func (w *TPCC) payment(c *Client) error {
	r := c.Rand
	wh, d := 1+r.Uint64N(w.Warehouses), 1+r.Uint64N(w.Districts)
	cw, cd, cu := wh, d, 1+r.Uint64N(w.Customers)
	if w.Warehouses > 1 && r.IntN(100) < 15 {
		cw, cd = w.otherWarehouse(r, wh), 1+r.Uint64N(w.Districts)
	}
	byName, last := r.IntN(100) < 60, int64(r.Uint64N(w.lastNames()))
	h := 1 + r.Int64N(5000)
	return expect(c.Do(func(tx *check.Tx) error {
		if err := tx.Add(tWarehouse, wKey(wh), wYTD, h); err != nil {
			return err
		}
		if _, err := get(tx, tWarehouse, wKey(wh), txn.Col(wName)); err != nil {
			return err
		}
		if err := tx.Add(tDistrict, dKey(wh, d), dYTD, h); err != nil {
			return err
		}
		if _, err := get(tx, tDistrict, dKey(wh, d), txn.Col(dName)); err != nil {
			return err
		}
		ck := cKey(cw, cd, cu)
		if byName {
			lo := lastKey(int64(cw), int64(cd), last)
			recs, err := tx.IndexScan(tCustomer, 0, lo, keys.Succ(lo), txn.Cols(cCredit, cDiscount), 0, nil)
			if err != nil {
				return err
			}
			if len(recs) == 0 {
				return fmt.Errorf("tpcc: no customer named %d in district %d/%d", last, cw, cd)
			}
			ck = recs[(len(recs)-1)/2].PK
		} else if _, err := get(tx, tCustomer, ck, txn.Cols(cCredit, cLast)); err != nil {
			return err
		}
		w.think(c)
		return errors.Join(
			tx.Add(tCustomer, ck, cBalance, -h),
			tx.Add(tCustomer, ck, cYTDPayment, h),
			tx.Add(tCustomer, ck, cPaymentCnt, 1),
			tx.Insert(tHistory, keys.U64(c.DB.NextID(tHistory)), values(int64(keys.ParseU64(ck)&(1<<32-1)), int64(cd), int64(cw), h)),
		)
	}))
}

// audit checks TPC-C consistency conditions 1–4 for one warehouse in a
// read-only transaction, which sees a single committed snapshot.
func (w *TPCC) audit(c *Client) error {
	wh := 1 + c.Rand.Uint64N(w.Warehouses)
	return expect(c.Do(func(tx *check.Tx) error {
		wr, err := get(tx, tWarehouse, wKey(wh), txn.Col(wYTD))
		if err != nil {
			return err
		}
		ds, err := tx.Scan(tDistrict, dKey(wh, 0), dKey(wh+1, 0), txn.Cols(dYTD, dNextOID), 0, nil)
		if err != nil {
			return err
		}
		var sum int64
		for _, d := range ds {
			sum += asInt(d.Row[dYTD])
		}
		if sum != wr[wYTD] {
			return fmt.Errorf("tpcc: condition 1: warehouse %d W_YTD %d != sum(D_YTD) %d", wh, wr[wYTD], sum)
		}
		for _, dr := range ds {
			d := keys.ParseU64(dr.PK) & 0xff
			w.think(c)
			os, err := tx.Scan(tOrders, oKey(wh, d, 0), oKey(wh, d+1, 0), txn.Col(oOLCnt), 0, nil)
			if err != nil {
				return err
			}
			nos, err := tx.Scan(tNewOrder, oKey(wh, d, 0), oKey(wh, d+1, 0), 0, 0, nil)
			if err != nil {
				return err
			}
			ols, err := tx.Scan(tOrderLine, olKey(wh, d, 0, 0), olKey(wh, d+1, 0, 0), 0, 0, nil)
			if err != nil {
				return err
			}
			next := uint64(asInt(dr.Row[dNextOID]))
			oid := func(k string) uint64 { return keys.ParseU64(k) & (1<<32 - 1) }
			maxO, maxNO, minNO := uint64(0), uint64(0), uint64(0)
			var lines int64
			for _, o := range os {
				maxO = oid(o.PK)
				lines += asInt(o.Row[oOLCnt])
			}
			if len(nos) > 0 {
				minNO, maxNO = oid(nos[0].PK), oid(nos[len(nos)-1].PK)
			}
			if next-1 != maxO || maxO != maxNO {
				return fmt.Errorf("tpcc: condition 2: district %d/%d D_NEXT_O_ID-1 %d, max(O_ID) %d, max(NO_O_ID) %d", wh, d, next-1, maxO, maxNO)
			}
			if len(nos) > 0 && maxNO-minNO+1 != uint64(len(nos)) {
				return fmt.Errorf("tpcc: condition 3: district %d/%d new orders %d..%d but %d rows", wh, d, minNO, maxNO, len(nos))
			}
			if lines != int64(len(ols)) {
				return fmt.Errorf("tpcc: condition 4: district %d/%d sum(O_OL_CNT) %d != %d order lines", wh, d, lines, len(ols))
			}
		}
		return nil
	}))
}
