void foo(int N, int *A) {
  int TSize = 4;
  int T[4];
  for (int I = 0; I < TSize; ++I)
    T[I] = I;
#pragma spf region
#pragma omp parallel
  {
#pragma omp for default(shared)
    for (int I = 0; I < N; ++I) {
      A[I] = I;
      for (int J = 0; J < TSize; ++J)
        A[I] = A[I] + T[J];
    }
  }
}
