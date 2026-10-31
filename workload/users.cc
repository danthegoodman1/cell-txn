#include "users.h"

#include <algorithm>
#include <cstdio>

#include "detmath.h"
#include "keys.h"

namespace celltnx::workload {

namespace {

enum { kPay, kDeposit, kProfile, kAudit, kSame, kRead };
enum { uName, uEmail, uWallet };
enum { tUsers, tAudit };

}  // namespace

Zipf::Zipf(uint64_t n, double theta) : n_(n), theta_(theta) {
  if (theta == 0) return;
  auto zeta = [theta](uint64_t k) {
    double s = 0.0;
    for (uint64_t i = 1; i <= k; i++) s += 1 / detmath::Pow(double(i), theta);
    return s;
  };
  zetan_ = zeta(n);
  alpha_ = 1 / (1 - theta);
  eta_ = (1 - detmath::Pow(2 / double(n), 1 - theta)) / (1 - zeta(2) / zetan_);
}

uint64_t Zipf::Next(Rand &r) const {
  if (theta_ == 0 || n_ < 2) return r.Uint64N(n_);
  double u = r.Float64();
  double uz = u * zetan_;
  if (uz < 1) return 0;
  if (uz < 1 + detmath::Pow(0.5, theta_)) return 1;
  return std::min(n_ - 1, detmath::FloatToUint64(double(n_) * detmath::Pow(eta_ * u - eta_ + 1, alpha_)));
}

Users::Users(uint64_t rows, double theta, std::array<int, kMix> mix, double think)
    : Rows(rows), Theta(theta), Mix(mix), Think(think), zipf_(rows, theta) {}

std::unique_ptr<Users> NewUsers(uint64_t rows, double theta, std::array<int, Users::kMix> mix, double think) {
  return std::make_unique<Users>(rows, theta, mix, think);
}

std::unique_ptr<Workload> NewUsersFrom(Rand &r) {
  std::array<int, Users::kMix> mix{};
  for (int &x : mix) x = int(r.IntN(6));
  mix[kPay]++;
  uint64_t rows = uint64_t(2 + r.IntN(30));
  double theta = r.Float64() * 0.99;
  double think = r.Float64() * 0.3;
  return NewUsers(rows, theta, mix, think);
}

std::string Users::String() const {
  char theta[64], think[64];
  snprintf(theta, sizeof theta, "%.2f", Theta);
  snprintf(think, sizeof think, "%.2f", Think);
  return "rows=" + std::to_string(Rows) + " theta=" + theta + " mix=" + FormatInts(Mix) + " think=" + think;
}

std::vector<Schema> Users::Schemas() {
  return {
      Schema{.name = "users",
             .cols = {"name", "email", "wallet"},
             .checks = {Check{"wallet >= 0", Col(uWallet), [](Row r) { return AsInt(r[uWallet]) >= 0; }}}},
      Schema{.name = "audit", .cols = {"user", "email"}},
  };
}

Status Users::Load(Client &c) {
  return c.Do([&](check::Tx &tx) -> Status {
    for (uint64_t id = 0; id < Rows; id++) {
      int64_t name = c.Uniq();
      int64_t email = c.Uniq();
      int64_t wallet = c.Rand.Int64N(30);
      std::vector<Value> row = {Value::Int(name), Value::Int(email), Value::Int(wallet)};
      if (Status st = tx.Insert(tUsers, keys::U64(id), row); !st.ok()) return st;
    }
    return {};
  });
}

int Users::Pick(Rand &r) const {
  int64_t total = 0;
  for (int x : Mix) total += x;
  int64_t n = r.IntN(total);
  for (int i = 0; i < kMix; i++) {
    if (n < Mix[i]) return i;
    n -= Mix[i];
  }
  throw InvariantError("unreachable");
}

void Users::ThinkFor(Client &c) const {
  if (c.Rand.Float64() < Think) c.Env->Sleep(1 + c.Rand.Int64N(50));
}

Status Users::Step(Client &c) {
  Rand &r = c.Rand;
  uint64_t n = zipf_.Next(r);
  std::string id = keys::U64(n);
  int64_t x = 1 + r.Int64N(5);
  int kind = Pick(r);
  return Expect(c.Do([&](check::Tx &tx) -> Status {
    std::vector<Value> u;
    bool ok = false;
    auto missing = [&](const Status &st) {
      return Internal("users: get " + std::to_string(n) + ": " + (ok ? "true" : "false") + " " +
                      (st.ok() ? "<nil>" : st.Message()));
    };
    switch (kind) {
      case kPay:
        return tx.Add(tUsers, id, uWallet, -x);
      case kDeposit:
        return tx.Add(tUsers, id, uWallet, x);
      case kProfile: {
        std::vector<Value> v(3);
        v[uEmail] = Value::Int(c.Uniq());
        return tx.Set(tUsers, id, Col(uEmail), v);
      }
      case kAudit: {
        Status st = tx.Get(tUsers, id, Col(uEmail), &u, &ok);
        if (!st.ok() || !ok) return missing(st);
        ThinkFor(c);
        std::string pk = keys::U64(c.DB->NextID(tAudit));
        std::vector<Value> row = {Value::Int(int64_t(n)), u[uEmail]};
        return tx.Insert(tAudit, pk, row);
      }
      case kSame: {
        Status st = tx.Get(tUsers, id, Col(uWallet), &u, &ok);
        if (!st.ok() || !ok) return missing(st);
        ThinkFor(c);
        if (u[uWallet].type != Type::kInt) throw InvariantError("users: wallet is not an int64");
        std::vector<Value> v(3);
        v[uWallet] = Value::Int(u[uWallet].i + 1);
        return tx.Set(tUsers, id, Col(uWallet), v);
      }
      default: {
        // Both keys are drawn before the loop, so the second draw
        // precedes the first read.
        std::string pks[] = {id, keys::U64(zipf_.Next(r))};
        for (const std::string &pk : pks) {
          if (Status st = tx.Get(tUsers, pk, Col(uName) | Col(uEmail) | Col(uWallet), &u, &ok); !st.ok()) return st;
          ThinkFor(c);
        }
        return {};
      }
    }
  }));
}

}  // namespace celltnx::workload
