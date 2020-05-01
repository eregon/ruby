prelude <<RUBY
def fib n
  if n < 3
    1
  else
    fib(n-1) + fib(n-2)
  end
end
RUBY

report 'app_fib', <<RUBY
fib(34)
RUBY
