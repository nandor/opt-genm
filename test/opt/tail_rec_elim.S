# RUN: %opt - -pass=tail-rec-elim -emit=llir

tail_rec_loop:
  .call            c
  .args            i64, i64
  .visibility      global_default

  arg.i64          $0, 0
  arg.i64          $1, 1
  mov.i64          $2, 0
  cmp.i32.eq       $3, $1, $2
  jf               $3, .Lfalse

.Ltrue:
  ret.i64          $1

.Lfalse:
  mul.i64          $4, $0, $0
  add.i64          $6, $4, $1
  mov.i64          $7, 1
  sub.i64          $8, $0, $7
  mov.i64          $9, tail_rec_loop
  tcall.i32.c      $9, $8, $6
  .end
