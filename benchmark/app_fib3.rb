require 'benchmark_driver'

Benchmark.driver do |x|
  x.prelude <<~RUBY
    def fib n
      if n < 3
        1
      else
        fib(n-1) + fib(n-2)
      end
    end
  RUBY

  x.report 'app_fib', 'fib(34)'
end
