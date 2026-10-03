package sqlgms

import (
	"fmt"
	"math/big"
	"reflect"
	"strings"
	"time"
	"unicode/utf8"

	"github.com/dolthub/go-mysql-server/sql"
	"github.com/dolthub/go-mysql-server/sql/types"
	"github.com/shopspring/decimal"

	"cell-tnx/keys"
	"cell-tnx/txn"
)

// toCore converts an engine value into a stored value: nil, int64, uint64,
// float64 or string. Times are stored as UTC microseconds, decimals and
// JSON as text.
func toCore(v any) (txn.Value, error) {
	switch x := v.(type) {
	case nil:
		return nil, nil
	case int8:
		return int64(x), nil
	case int16:
		return int64(x), nil
	case int32:
		return int64(x), nil
	case int64:
		return x, nil
	case int:
		return int64(x), nil
	case uint8:
		return uint64(x), nil
	case uint16:
		return uint64(x), nil
	case uint32:
		return uint64(x), nil
	case uint64:
		return x, nil
	case uint:
		return uint64(x), nil
	case float32:
		return float64(x), nil
	case float64:
		return x, nil
	case bool:
		if x {
			return int64(1), nil
		}
		return int64(0), nil
	case string:
		return x, nil
	case []byte:
		return string(x), nil
	case decimal.Decimal:
		return x.String(), nil
	case time.Time:
		return x.UTC().UnixMicro(), nil
	case types.Timespan:
		return int64(x), nil
	case sql.JSONWrapper:
		return types.JsonToMySqlString(x)
	}
	return nil, fmt.Errorf("sqlgms: unsupported value type %T", v)
}

// fromCore converts a stored value back into the engine type for typ.
func fromCore(typ sql.Type, v txn.Value) (any, error) {
	if v == nil {
		return nil, nil
	}
	switch {
	case types.IsTime(typ):
		return time.UnixMicro(v.(int64)).UTC(), nil
	case types.IsTimespan(typ):
		return types.Timespan(v.(int64)), nil
	case types.IsDecimal(typ):
		return decimal.NewFromString(v.(string))
	case types.IsJSON(typ):
		doc, _, err := types.JSON.Convert(sql.NewEmptyContext(), v.(string))
		return doc, err
	}
	rt := typ.ValueType()
	rv := reflect.ValueOf(v)
	if rt == nil || !rv.CanConvert(rt) {
		return nil, fmt.Errorf("sqlgms: cannot convert %T to %v", v, typ)
	}
	return rv.Convert(rt).Interface(), nil
}

func toCoreRow(row sql.Row) ([]txn.Value, error) {
	out := make([]txn.Value, len(row))
	for i, v := range row {
		var err error
		if out[i], err = toCore(v); err != nil {
			return nil, err
		}
	}
	return out, nil
}

func fromCoreRow(sch sql.Schema, row []txn.Value) (sql.Row, error) {
	out := make(sql.Row, len(row))
	for i, v := range row {
		var err error
		if out[i], err = fromCore(sch[i].Type, v); err != nil {
			return nil, err
		}
	}
	return out, nil
}

// appendKey appends the order-preserving encoding of a stored value of
// type typ. Strings sort by their collation's rune weights, so collation
// equality and key equality agree.
func appendKey(b []byte, typ sql.Type, v txn.Value) ([]byte, error) {
	switch x := v.(type) {
	case nil:
		return keys.AppendNull(b), nil
	case int64:
		return keys.AppendInt(b, x), nil
	case uint64:
		return keys.AppendUint(b, x), nil
	case float64:
		return keys.AppendFloat(b, x), nil
	case string:
		switch {
		case types.IsDecimal(typ):
			d, err := decimal.NewFromString(x)
			if err != nil {
				return nil, err
			}
			return appendDecimal(b, d), nil
		case types.IsJSON(typ):
			return nil, fmt.Errorf("sqlgms: JSON columns cannot be keys")
		}
		if st, ok := typ.(sql.StringType); ok && !types.IsBinaryType(typ) {
			return appendCollated(b, x, st.Collation()), nil
		}
		return keys.AppendString(b, x), nil
	}
	return nil, fmt.Errorf("sqlgms: cannot key %T", v)
}

// appendCollated encodes s by its collation weights, four bytes per rune,
// escaped and terminated like keys.AppendString.
func appendCollated(b []byte, s string, c sql.CollationID) []byte {
	sorter := c.Sorter()
	if c == sql.Collation_binary || sorter == nil {
		return keys.AppendString(b, s)
	}
	if c.PadAttribute() == "PAD SPACE" {
		s = strings.TrimRight(s, " ")
	}
	var w []byte
	for _, r := range s {
		if r == utf8.RuneError {
			r = 0
		}
		u := uint32(sorter(r)) ^ 1<<31
		w = append(w, byte(u>>24), byte(u>>16), byte(u>>8), byte(u))
	}
	return keys.AppendString(b, string(w))
}

// appendDecimal encodes a decimal as sign, decimal exponent and digits,
// inverting the exponent and digits of negatives so byte order is numeric
// order.
func appendDecimal(b []byte, d decimal.Decimal) []byte {
	b = append(b, 0x02)
	sign := d.Sign()
	if sign == 0 {
		return append(b, 0x80)
	}
	digits := new(big.Int).Abs(d.Coefficient()).String()
	exp := int64(d.Exponent())
	trimmed := strings.TrimRight(digits, "0")
	exp += int64(len(digits) - len(trimmed))
	e := uint32(int32(int64(len(trimmed)) + exp))
	if sign > 0 {
		b = append(b, 0x81, byte(e>>24)^0x80, byte(e>>16), byte(e>>8), byte(e))
		return append(append(b, trimmed...), 0)
	}
	e = ^e
	b = append(b, 0x7f, byte(e>>24)^0x80, byte(e>>16), byte(e>>8), byte(e))
	for i := 0; i < len(trimmed); i++ {
		b = append(b, 0xff-trimmed[i])
	}
	return append(b, 0xff)
}

// encodeKey encodes the columns at positions cols of a stored row.
func encodeKey(sch sql.Schema, cols []int, row []txn.Value) (string, bool, error) {
	var b []byte
	nonNull := true
	for _, c := range cols {
		var err error
		if b, err = appendKey(b, sch[c].Type, row[c]); err != nil {
			return "", false, err
		}
		nonNull = nonNull && row[c] != nil
	}
	return string(b), nonNull, nil
}

// keyWidth is the encoded width of a column's keys when it is fixed.
func keyWidth(typ sql.Type) int {
	switch {
	case types.IsInteger(typ), types.IsFloat(typ), types.IsTime(typ), types.IsTimespan(typ),
		types.IsEnum(typ), types.IsSet(typ), types.IsBit(typ), types.IsYear(typ):
		return 9
	}
	return 0
}

// bucketPrefix covers the fixed-width leading key columns, so buckets
// split on the last of them, falling back to 8 bytes.
func bucketPrefix(sch sql.Schema, cols []int) int {
	n := 0
	for _, c := range cols {
		w := keyWidth(sch[c].Type)
		if w == 0 {
			break
		}
		n += w
	}
	if n == 0 {
		return 8
	}
	return min(n, 64)
}
