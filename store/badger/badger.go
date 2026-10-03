// Package badger persists cell-tnx commits in Badger's managed mode.
//
// Each commit becomes one Badger transaction at the commit's timestamp.
// Badger applies a transaction atomically and the writer submits commits in
// timestamp order, so a crash leaves a prefix of the commits; the crash
// test checks this against real kill -9s.
package badger

import (
	"encoding/binary"
	"errors"
	"fmt"
	"math"

	bdg "github.com/dgraph-io/badger/v4"

	"cell-tnx/txn"
)

// Store implements txn.Store.
type Store struct {
	db *bdg.DB
}

// Open opens or creates a store in dir.
func Open(dir string) (*Store, error) {
	opts := bdg.DefaultOptions(dir).
		WithLogger(nil).
		WithDetectConflicts(false).
		WithSyncWrites(false).
		WithNumVersionsToKeep(1)
	db, err := bdg.OpenManaged(opts)
	if err != nil {
		return nil, err
	}
	return &Store{db: db}, nil
}

// Close closes the store.
func (s *Store) Close() error { return s.db.Close() }

// Last returns the newest commit timestamp in the store.
func (s *Store) Last() uint64 { return s.db.MaxVersion() }

// Write submits each commit as one transaction, in order, and waits for
// all of them to reach Badger's write-ahead log.
func (s *Store) Write(batch []txn.Commit) error {
	done := make(chan error, len(batch))
	for _, c := range batch {
		tx := s.db.NewTransactionAt(c.Ts, true)
		for _, w := range c.Writes {
			k := rowKey(w.Table, w.Key)
			var err error
			if w.Row == nil {
				err = tx.Delete(k)
			} else {
				err = tx.Set(k, encodeRow(w.Row))
			}
			if err != nil {
				tx.Discard()
				return fmt.Errorf("badger: commit %d: %w", c.Ts, err)
			}
		}
		if err := tx.CommitAt(c.Ts, func(err error) { done <- err }); err != nil {
			return err
		}
	}
	var errs []error
	for range batch {
		errs = append(errs, <-done)
	}
	return errors.Join(errs...)
}

// Sync makes every written commit durable and lets Badger discard the
// versions they replaced.
func (s *Store) Sync() error {
	if err := s.db.Sync(); err != nil {
		return err
	}
	s.db.SetDiscardTs(s.db.MaxVersion())
	return nil
}

// Load passes the newest image of every live row to put.
func (s *Store) Load(put func(tbl int, key string, row []txn.Value)) error {
	tx := s.db.NewTransactionAt(s.db.MaxVersion(), false)
	defer tx.Discard()
	it := tx.NewIterator(bdg.DefaultIteratorOptions)
	defer it.Close()
	for it.Rewind(); it.Valid(); it.Next() {
		item := it.Item()
		k := item.Key()
		v, err := item.ValueCopy(nil)
		if err != nil {
			return err
		}
		row, err := decodeRow(v)
		if err != nil {
			return fmt.Errorf("badger: key %x: %w", k, err)
		}
		put(int(binary.BigEndian.Uint32(k)), string(k[4:]), row)
	}
	return nil
}

func rowKey(tbl int, key string) []byte {
	b := binary.BigEndian.AppendUint32(make([]byte, 0, 4+len(key)), uint32(tbl))
	return append(b, key...)
}

const (
	tagNil = iota
	tagInt
	tagUint
	tagFloat
	tagString
)

func encodeRow(row []txn.Value) []byte {
	b := binary.AppendUvarint(nil, uint64(len(row)))
	for _, v := range row {
		switch x := v.(type) {
		case nil:
			b = append(b, tagNil)
		case int64:
			b = binary.BigEndian.AppendUint64(append(b, tagInt), uint64(x))
		case uint64:
			b = binary.BigEndian.AppendUint64(append(b, tagUint), x)
		case float64:
			b = binary.BigEndian.AppendUint64(append(b, tagFloat), math.Float64bits(x))
		case string:
			b = binary.AppendUvarint(append(b, tagString), uint64(len(x)))
			b = append(b, x...)
		default:
			panic(fmt.Sprintf("badger: unsupported value %T", v))
		}
	}
	return b
}

var errCorrupt = errors.New("badger: corrupt row")

func decodeRow(b []byte) ([]txn.Value, error) {
	n, k := binary.Uvarint(b)
	if k <= 0 {
		return nil, errCorrupt
	}
	b = b[k:]
	row := make([]txn.Value, n)
	for i := range row {
		if len(b) == 0 {
			return nil, errCorrupt
		}
		tag := b[0]
		b = b[1:]
		switch tag {
		case tagNil:
		case tagInt, tagUint, tagFloat:
			if len(b) < 8 {
				return nil, errCorrupt
			}
			u := binary.BigEndian.Uint64(b)
			b = b[8:]
			switch tag {
			case tagInt:
				row[i] = int64(u)
			case tagUint:
				row[i] = u
			default:
				row[i] = math.Float64frombits(u)
			}
		case tagString:
			l, k := binary.Uvarint(b)
			if k <= 0 || uint64(len(b)-k) < l {
				return nil, errCorrupt
			}
			row[i] = string(b[k : k+int(l)])
			b = b[k+int(l):]
		default:
			return nil, errCorrupt
		}
	}
	if len(b) != 0 {
		return nil, errCorrupt
	}
	return row, nil
}
