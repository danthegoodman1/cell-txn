package badger

import (
	"slices"
	"testing"

	"cell-tnx/txn"
)

func TestRowCodec(t *testing.T) {
	row := []txn.Value{nil, int64(-5), uint64(1 << 63), 3.5, "", "a\x00b"}
	got, err := decodeRow(encodeRow(row))
	if err != nil || !slices.Equal(got, row) {
		t.Fatalf("round trip = %v, %v; want %v", got, err, row)
	}
	for i := range len(encodeRow(row)) {
		if _, err := decodeRow(encodeRow(row)[:i]); err == nil {
			t.Fatalf("truncated row of %d bytes decoded", i)
		}
	}
}

func w(tbl int, key string, v txn.Value) txn.Write {
	return txn.Write{Table: tbl, Key: key, Row: []txn.Value{v}}
}

// Writes persist and reload; deletes and later images win.
func TestStoreRoundTrip(t *testing.T) {
	dir := t.TempDir()
	st, err := Open(dir)
	if err != nil {
		t.Fatal(err)
	}
	err = st.Write([]txn.Commit{
		{Ts: 1, Writes: []txn.Write{w(0, "a", int64(1)), w(0, "b", int64(2)), w(1, "a", "x")}},
		{Ts: 2, Writes: []txn.Write{{Table: 0, Key: "a"}, w(0, "b", int64(3))}},
	})
	if err == nil {
		err = st.Sync()
	}
	if err != nil {
		t.Fatal(err)
	}
	st.Close()

	st, err = Open(dir)
	if err != nil {
		t.Fatal(err)
	}
	defer st.Close()
	if st.Last() != 2 {
		t.Fatalf("last = %d, want 2", st.Last())
	}
	type kv struct {
		tbl int
		key string
		row []txn.Value
	}
	var got []kv
	st.Load(func(tbl int, key string, row []txn.Value) { got = append(got, kv{tbl, key, row}) })
	if len(got) != 2 || got[0].key != "b" || got[0].row[0] != int64(3) || got[1].tbl != 1 || got[1].row[0] != "x" {
		t.Fatalf("loaded %v", got)
	}
}
