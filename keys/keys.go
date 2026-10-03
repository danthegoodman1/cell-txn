// Package keys encodes values into byte strings whose byte order matches
// the values' order. Every encoding is prefix-free, so encoded values can be
// concatenated into composite keys that sort component by component.
package keys

import (
	"encoding/binary"
	"math"
)

// U64 returns the 8-byte big-endian encoding of x.
func U64(x uint64) string {
	var b [8]byte
	binary.BigEndian.PutUint64(b[:], x)
	return string(b[:])
}

// ParseU64 decodes a key produced by U64.
func ParseU64(k string) uint64 { return binary.BigEndian.Uint64([]byte(k[:8])) }

// Tags distinguish NULL from values; NULL sorts first, as in MySQL.
const (
	tagNull  = 0x01
	tagValue = 0x02
)

// AppendNull appends a NULL.
func AppendNull(b []byte) []byte { return append(b, tagNull) }

// AppendInt appends a signed integer.
func AppendInt(b []byte, x int64) []byte {
	b = append(b, tagValue)
	return binary.BigEndian.AppendUint64(b, uint64(x)^1<<63)
}

// AppendUint appends an unsigned integer.
func AppendUint(b []byte, x uint64) []byte {
	b = append(b, tagValue)
	return binary.BigEndian.AppendUint64(b, x)
}

// AppendFloat appends a float; -0 and +0 encode equally.
func AppendFloat(b []byte, f float64) []byte {
	if f == 0 {
		f = 0
	}
	u := math.Float64bits(f)
	if u&(1<<63) != 0 {
		u = ^u
	} else {
		u |= 1 << 63
	}
	b = append(b, tagValue)
	return binary.BigEndian.AppendUint64(b, u)
}

// AppendString appends a string, escaping 0x00 as 0x00 0xff and ending
// with 0x00 0x01, so shorter strings sort before their extensions.
func AppendString(b []byte, s string) []byte {
	b = append(b, tagValue)
	for i := 0; i < len(s); i++ {
		if s[i] == 0 {
			b = append(b, 0, 0xff)
		} else {
			b = append(b, s[i])
		}
	}
	return append(b, 0, 0x01)
}

// Succ returns the smallest key greater than every key with prefix k.
// It returns "" (unbounded) when no such key exists.
func Succ(k string) string {
	b := []byte(k)
	for i := len(b) - 1; i >= 0; i-- {
		if b[i] != 0xff {
			b[i]++
			return string(b[:i+1])
		}
	}
	return ""
}

// Next returns the smallest key greater than k.
func Next(k string) string { return k + "\x00" }
