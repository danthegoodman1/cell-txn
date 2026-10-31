#include "tpcc.h"

#include <algorithm>
#include <initializer_list>

#include "gofmt.h"
#include "keys.h"

namespace celltnx::workload {

namespace {

// Tables and columns.
enum { tWarehouse, tDistrict, tCustomer, tHistory, tOrders, tNewOrder, tOrderLine, tItem, tStock };
enum { wTax, wYTD, wName };
enum { dTax, dYTD, dNextOID, dName };
enum { cDiscount, cCredit, cLast, cBalance, cYTDPayment, cPaymentCnt, cWID, cDID };
enum { oCID, oOLCnt, oAllLocal };
enum { olIID, olSupplyWID, olQuantity, olAmount };
enum { sQuantity, sYTD, sOrderCnt, sRemoteCnt };

// Keys pack composite primary keys into 8 bytes so key order matches
// TPC-C's.
std::string WKey(uint64_t w) { return keys::U64(w); }
std::string DKey(uint64_t w, uint64_t d) { return keys::U64(w << 8 | d); }
std::string CKey(uint64_t w, uint64_t d, uint64_t c) { return keys::U64(w << 40 | d << 32 | c); }
std::string OKey(uint64_t w, uint64_t d, uint64_t o) { return keys::U64(w << 40 | d << 32 | o); }
std::string OLKey(uint64_t w, uint64_t d, uint64_t o, uint64_t ol) {
  return keys::U64(w << 44 | d << 36 | o << 4 | ol);
}
std::string SKey(uint64_t w, uint64_t i) { return keys::U64(w << 32 | i); }

// LastKey encodes the customer index key (warehouse, district, last name).
void AppendLastKey(std::string &k, int64_t w, int64_t d, int64_t last) {
  keys::AppendInt(k, w);
  keys::AppendInt(k, d);
  keys::AppendInt(k, last);
}

std::string LastKey(int64_t w, int64_t d, int64_t last) {
  std::string k;
  AppendLastKey(k, w, d, last);
  return k;
}

template <class C>
std::vector<Value> Values(const C &xs) {
  std::vector<Value> out;
  out.reserve(xs.size());
  for (int64_t x : xs) out.push_back(Value::Int(x));
  return out;
}

std::vector<Value> Values(std::initializer_list<int64_t> xs) { return Values<std::initializer_list<int64_t>>(xs); }

// Join stands in for errors.Join over statuses already evaluated in order:
// it returns the first failure. Writes fail only with codes Do and Expect
// treat alike, so the first one decides the outcome as the joined error
// does.
template <class... S>
Status Join(S... sts) {
  Status out;
  ((out.ok() && !sts.ok() ? void(out = std::move(sts)) : void()), ...);
  return out;
}

std::string Hex(std::string_view s) {
  static const char digits[] = "0123456789abcdef";
  std::string out;
  for (unsigned char c : s) {
    out += digits[c >> 4];
    out += digits[c & 15];
  }
  return out;
}

// Get reads a row that must exist, as int64s.
Status Get(check::Tx &tx, int tbl, std::string_view pk, ColMask cols, std::vector<int64_t> *out) {
  std::vector<Value> row;
  bool ok = false;
  Status st = tx.Get(tbl, pk, cols, &row, &ok);
  if (st.ok() && !ok) st = Internal("tpcc: table " + std::to_string(tbl) + " row " + Hex(pk) + " missing");
  out->resize(row.size());
  for (size_t i = 0; i < row.size(); i++) (*out)[i] = AsInt(row[i]);
  return st;
}

}  // namespace

TPCC::TPCC(uint64_t warehouses, uint64_t districts, uint64_t customers, uint64_t items, int max_lines)
    : Warehouses(warehouses), Districts(districts), Customers(customers), Items(items), MaxLines(max_lines) {}

std::unique_ptr<TPCC> NewTPCC(uint64_t warehouses, uint64_t districts, uint64_t customers, uint64_t items,
                              int max_lines) {
  return std::make_unique<TPCC>(warehouses, districts, customers, items, max_lines);
}

std::unique_ptr<Workload> NewTPCCFrom(Rand &r) {
  uint64_t warehouses = uint64_t(1 + r.IntN(2));
  uint64_t districts = uint64_t(1 + r.IntN(4));
  uint64_t customers = uint64_t(2 + r.IntN(10));
  uint64_t items = uint64_t(5 + r.IntN(40));
  int max_lines = int(1 + r.IntN(6));
  std::unique_ptr<TPCC> w = NewTPCC(warehouses, districts, customers, items, max_lines);
  w->Mix[2] = int(1 + r.IntN(10));
  w->Think = r.Float64() * 0.2;
  return w;
}

std::string TPCC::Detail() {
  return "&{Warehouses:" + std::to_string(Warehouses) + " Districts:" + std::to_string(Districts) +
         " Customers:" + std::to_string(Customers) + " Items:" + std::to_string(Items) +
         " MaxLines:" + std::to_string(MaxLines) + " Mix:" + GoInts(Mix) + " Think:" + GoFloat(Think) + "}";
}

std::vector<Schema> TPCC::Schemas() {
  Index by_last{.name = "c_last",
                .cols = Col(cWID) | Col(cDID) | Col(cLast),
                .key =
                    [](Row r, std::string *out) {
                      AppendLastKey(*out, AsInt(r[cWID]), AsInt(r[cDID]), AsInt(r[cLast]));
                      return false;
                    },
                .bucket_prefix = 27};
  return {
      Schema{.name = "warehouse", .cols = {"w_tax", "w_ytd", "w_name"}},
      Schema{.name = "district", .cols = {"d_tax", "d_ytd", "d_next_o_id", "d_name"}},
      Schema{.name = "customer",
             .cols = {"c_discount", "c_credit", "c_last", "c_balance", "c_ytd_payment", "c_payment_cnt", "c_w_id",
                      "c_d_id"},
             .indexes = {by_last}},
      Schema{.name = "history", .cols = {"h_c_id", "h_d_id", "h_w_id", "h_amount"}},
      Schema{.name = "orders", .cols = {"o_c_id", "o_ol_cnt", "o_all_local"}},
      Schema{.name = "new_order", .cols = {"no_flag"}},
      Schema{.name = "order_line", .cols = {"ol_i_id", "ol_supply_w_id", "ol_quantity", "ol_amount"}},
      Schema{.name = "item", .cols = {"i_price"}},
      Schema{.name = "stock", .cols = {"s_quantity", "s_ytd", "s_order_cnt", "s_remote_cnt"}},
  };
}

// LastNames is how many distinct last names each district's customers
// share, so a by-name lookup usually finds several customers.
uint64_t TPCC::LastNames() const { return std::max<uint64_t>(1, Customers / 3); }

Status TPCC::Load(Client &c) {
  using Fn = std::function<Status(check::Tx &)>;
  Rand &r = c.Rand;
  std::vector<Fn> rows;
  auto ins = [&rows](int tbl, std::string pk, std::vector<Value> row) {
    rows.push_back([tbl, pk = std::move(pk), row = std::move(row)](check::Tx &tx) { return tx.Insert(tbl, pk, row); });
  };
  for (uint64_t i = 1; i <= Items; i++) {
    int64_t price = 1 + r.Int64N(100);
    ins(tItem, keys::U64(i), Values({price}));
  }
  for (uint64_t wh = 1; wh <= Warehouses; wh++) {
    for (uint64_t i = 1; i <= Items; i++) {
      int64_t qty = 10 + r.Int64N(91);
      ins(tStock, SKey(wh, i), Values({qty, 0, 0, 0}));
    }
    for (uint64_t d = 1; d <= Districts; d++) {
      for (uint64_t cu = 1; cu <= Customers; cu++) {
        int64_t last = int64_t(cu % LastNames());
        int64_t discount = r.Int64N(5000);
        int64_t credit = r.Int64N(10) / 9;
        ins(tCustomer, CKey(wh, d, cu), Values({discount, credit, last, -10, 10, 1, int64_t(wh), int64_t(d)}));
      }
    }
  }
  // Each warehouse and its districts go in one transaction, after the rows
  // they refer to. Their random values are drawn as the transaction runs.
  for (uint64_t wh = 1; wh <= Warehouses; wh++) {
    std::vector<Fn> group;
    group.push_back([this, &c, &r, wh](check::Tx &tx) {
      int64_t tax = r.Int64N(2000);
      int64_t name = c.Uniq();
      return tx.Insert(tWarehouse, WKey(wh), Values({tax, 30000 * int64_t(Districts), name}));
    });
    for (uint64_t d = 1; d <= Districts; d++) {
      group.push_back([&c, &r, wh, d](check::Tx &tx) {
        int64_t tax = r.Int64N(2000);
        int64_t name = c.Uniq();
        return tx.Insert(tDistrict, DKey(wh, d), Values({tax, 30000, 1, name}));
      });
    }
    rows.push_back([group = std::move(group)](check::Tx &tx) -> Status {
      for (const Fn &f : group)
        if (Status st = f(tx); !st.ok()) return st;
      return {};
    });
  }
  for (size_t start = 0; start < rows.size();) {
    size_t n = std::min<size_t>(rows.size() - start, 1000);
    Status st = c.Do([&](check::Tx &tx) -> Status {
      for (size_t i = start; i < start + n; i++)
        if (Status st = rows[i](tx); !st.ok()) return st;
      return {};
    });
    if (!st.ok()) return st;
    start += n;
  }
  return {};
}

Status TPCC::Step(Client &c) {
  int64_t total = int64_t(Mix[0]) + Mix[1] + Mix[2];
  int64_t n = c.Rand.IntN(total);
  if (n < Mix[0]) return NewOrder(c);
  if (n < int64_t(Mix[0]) + Mix[1]) return Payment(c);
  return Audit(c);
}

void TPCC::ThinkFor(Client &c) const {
  if (c.Rand.Float64() < Think) c.Env->Sleep(1 + c.Rand.Int64N(50));
}

uint64_t TPCC::OtherWarehouse(Rand &r, uint64_t wh) const {
  uint64_t o = 1 + r.Uint64N(Warehouses - 1);
  if (o >= wh) o++;
  return o;
}

Status TPCC::NewOrder(Client &c) {
  Rand &r = c.Rand;
  uint64_t wh = 1 + r.Uint64N(Warehouses);
  uint64_t d = 1 + r.Uint64N(Districts);
  uint64_t cu = 1 + r.Uint64N(Customers);
  struct Line {
    uint64_t item, supply;
    int64_t qty;
  };
  std::vector<Line> lines(size_t(1 + r.IntN(MaxLines)));
  int64_t all_local = 1;
  for (Line &l : lines) {
    l.item = 1 + r.Uint64N(Items);
    l.supply = wh;
    l.qty = 1 + r.Int64N(10);
    if (Warehouses > 1 && r.IntN(100) == 0) {
      l.supply = OtherWarehouse(r, wh);
      all_local = 0;
    }
  }
  if (r.IntN(100) == 0) lines.back().item = Items + 1;  // unused item: roll back
  return Expect(c.Do([&](check::Tx &tx) -> Status {
    std::vector<int64_t> tmp, dist, s;
    if (Status st = Get(tx, tWarehouse, WKey(wh), Col(wTax), &tmp); !st.ok()) return st;
    if (Status st = Get(tx, tDistrict, DKey(wh, d), Col(dTax) | Col(dNextOID), &dist); !st.ok()) return st;
    uint64_t o = uint64_t(dist[dNextOID]);
    dist[dNextOID]++;
    if (Status st = tx.Set(tDistrict, DKey(wh, d), Col(dNextOID), Values(dist)); !st.ok()) return st;
    if (Status st = Get(tx, tCustomer, CKey(wh, d, cu), Col(cDiscount) | Col(cCredit) | Col(cLast), &tmp); !st.ok())
      return st;
    ThinkFor(c);
    if (Status st = tx.Insert(tOrders, OKey(wh, d, o), Values({int64_t(cu), int64_t(lines.size()), all_local}));
        !st.ok())
      return st;
    if (Status st = tx.Insert(tNewOrder, OKey(wh, d, o), Values({0})); !st.ok()) return st;
    for (size_t n = 0; n < lines.size(); n++) {
      const Line &l = lines[n];
      std::vector<Value> item;
      bool ok = false;
      if (Status st = tx.Get(tItem, keys::U64(l.item), Col(0), &item, &ok); !st.ok()) return st;
      if (!ok) return ErrAbort();
      std::string sk = SKey(l.supply, l.item);
      if (Status st = Get(tx, tStock, sk, Col(sQuantity), &s); !st.ok()) return st;
      if (s[sQuantity] -= l.qty; s[sQuantity] < 10) s[sQuantity] += 91;
      Status set = tx.Set(tStock, sk, Col(sQuantity), Values(s));
      Status ytd = tx.Add(tStock, sk, sYTD, l.qty);
      Status cnt = tx.Add(tStock, sk, sOrderCnt, 1);
      Status err = Join(std::move(set), std::move(ytd), std::move(cnt));
      if (err.ok() && l.supply != wh) err = tx.Add(tStock, sk, sRemoteCnt, 1);
      if (err.ok())
        err = tx.Insert(tOrderLine, OLKey(wh, d, o, n + 1),
                        Values({int64_t(l.item), int64_t(l.supply), l.qty, l.qty * AsInt(item[0])}));
      if (!err.ok()) return err;
    }
    return {};
  }));
}

Status TPCC::Payment(Client &c) {
  Rand &r = c.Rand;
  uint64_t wh = 1 + r.Uint64N(Warehouses);
  uint64_t d = 1 + r.Uint64N(Districts);
  uint64_t cw = wh, cd = d, cu = 1 + r.Uint64N(Customers);
  if (Warehouses > 1 && r.IntN(100) < 15) {
    cw = OtherWarehouse(r, wh);
    cd = 1 + r.Uint64N(Districts);
  }
  bool by_name = r.IntN(100) < 60;
  int64_t last = int64_t(r.Uint64N(LastNames()));
  int64_t h = 1 + r.Int64N(5000);
  return Expect(c.Do([&](check::Tx &tx) -> Status {
    std::vector<int64_t> tmp;
    if (Status st = tx.Add(tWarehouse, WKey(wh), wYTD, h); !st.ok()) return st;
    if (Status st = Get(tx, tWarehouse, WKey(wh), Col(wName), &tmp); !st.ok()) return st;
    if (Status st = tx.Add(tDistrict, DKey(wh, d), dYTD, h); !st.ok()) return st;
    if (Status st = Get(tx, tDistrict, DKey(wh, d), Col(dName), &tmp); !st.ok()) return st;
    std::string ck = CKey(cw, cd, cu);
    if (by_name) {
      std::string lo = LastKey(int64_t(cw), int64_t(cd), last);
      std::vector<Rec> recs;
      if (Status st = tx.IndexScan(tCustomer, 0, lo, keys::Succ(lo), Col(cCredit) | Col(cDiscount), 0, nullptr, &recs);
          !st.ok())
        return st;
      if (recs.empty())
        return Internal("tpcc: no customer named " + std::to_string(last) + " in district " + std::to_string(cw) +
                        "/" + std::to_string(cd));
      ck = std::string(recs[(recs.size() - 1) / 2].pk);
    } else if (Status st = Get(tx, tCustomer, ck, Col(cCredit) | Col(cLast), &tmp); !st.ok()) {
      return st;
    }
    ThinkFor(c);
    Status balance = tx.Add(tCustomer, ck, cBalance, -h);
    Status ytd = tx.Add(tCustomer, ck, cYTDPayment, h);
    Status cnt = tx.Add(tCustomer, ck, cPaymentCnt, 1);
    std::string hk = keys::U64(c.DB->NextID(tHistory));
    Status hist =
        tx.Insert(tHistory, hk, Values({int64_t(keys::ParseU64(ck) & 0xffffffff), int64_t(cd), int64_t(cw), h}));
    return Join(std::move(balance), std::move(ytd), std::move(cnt), std::move(hist));
  }));
}

// Audit checks TPC-C consistency conditions 1–4 for one warehouse in a
// read-only transaction, which sees a single committed snapshot.
Status TPCC::Audit(Client &c) {
  uint64_t wh = 1 + c.Rand.Uint64N(Warehouses);
  return Expect(c.Do([&](check::Tx &tx) -> Status {
    std::vector<int64_t> wr;
    if (Status st = Get(tx, tWarehouse, WKey(wh), Col(wYTD), &wr); !st.ok()) return st;
    std::vector<Rec> ds;
    if (Status st = tx.Scan(tDistrict, DKey(wh, 0), DKey(wh + 1, 0), Col(dYTD) | Col(dNextOID), 0, nullptr, &ds);
        !st.ok())
      return st;
    int64_t sum = 0;
    for (const Rec &d : ds) sum += AsInt(d.row[dYTD]);
    if (sum != wr[wYTD])
      return Internal("tpcc: condition 1: warehouse " + std::to_string(wh) + " W_YTD " + std::to_string(wr[wYTD]) +
                      " != sum(D_YTD) " + std::to_string(sum));
    std::vector<Rec> os, nos, ols;
    auto oid = [](std::string_view k) { return keys::ParseU64(k) & 0xffffffff; };
    for (const Rec &dr : ds) {
      uint64_t d = keys::ParseU64(dr.pk) & 0xff;
      ThinkFor(c);
      if (Status st = tx.Scan(tOrders, OKey(wh, d, 0), OKey(wh, d + 1, 0), Col(oOLCnt), 0, nullptr, &os); !st.ok())
        return st;
      if (Status st = tx.Scan(tNewOrder, OKey(wh, d, 0), OKey(wh, d + 1, 0), 0, 0, nullptr, &nos); !st.ok())
        return st;
      if (Status st = tx.Scan(tOrderLine, OLKey(wh, d, 0, 0), OLKey(wh, d + 1, 0, 0), 0, 0, nullptr, &ols); !st.ok())
        return st;
      uint64_t next = uint64_t(AsInt(dr.row[dNextOID]));
      uint64_t max_o = 0, max_no = 0, min_no = 0;
      int64_t lines = 0;
      for (const Rec &o : os) {
        max_o = oid(o.pk);
        lines += AsInt(o.row[oOLCnt]);
      }
      if (!nos.empty()) {
        min_no = oid(nos.front().pk);
        max_no = oid(nos.back().pk);
      }
      std::string where = std::to_string(wh) + "/" + std::to_string(d);
      if (next - 1 != max_o || max_o != max_no)
        return Internal("tpcc: condition 2: district " + where + " D_NEXT_O_ID-1 " + std::to_string(next - 1) +
                        ", max(O_ID) " + std::to_string(max_o) + ", max(NO_O_ID) " + std::to_string(max_no));
      if (!nos.empty() && max_no - min_no + 1 != nos.size())
        return Internal("tpcc: condition 3: district " + where + " new orders " + std::to_string(min_no) + ".." +
                        std::to_string(max_no) + " but " + std::to_string(nos.size()) + " rows");
      if (lines != int64_t(ols.size()))
        return Internal("tpcc: condition 4: district " + where + " sum(O_OL_CNT) " + std::to_string(lines) +
                        " != " + std::to_string(ols.size()) + " order lines");
    }
    return {};
  }));
}

}  // namespace celltnx::workload
