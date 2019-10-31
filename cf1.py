A = list(map(int, input().split()))
b = dict()
p = 89
k = 997
def fn(A, i, j):
	if i > j:
		return;
	if((i*p*p + j*p)%k) not in b:
		# print((i*p*p + j*p)%k)
		# print(i,j)
		print(A[i : j + 1])
		b[(i*p*p + j*p)%k] = True
	fn(A, i + 1, j)
	fn(A , i , j - 1)
fn(A, 0, len(A)-1)
