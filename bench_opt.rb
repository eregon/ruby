p Kernel.foo_c(42)
p Kernel.foo_builtin(42)
p Kernel.foo_pure_ruby(42)

p Kernel.foo_c(42, 17)
p Kernel.foo_builtin(42, 17)
p Kernel.foo_pure_ruby(42, 17)

puts RubyVM::InstructionSequence.of(Kernel.instance_method(:clone)).disasm
puts

puts RubyVM::InstructionSequence.of(GC.method(:stress=)).disasm
puts

puts RubyVM::InstructionSequence.of(Kernel.method(:foo_builtin)).disasm
puts

require 'benchmark_driver'

Benchmark.driver do |x|
  x.time = 1
  x.report 'Kernel.foo_c(42)'
  x.report 'Kernel.foo_builtin(42)'
  x.report 'Kernel.foo_pure_ruby(42)'
end

Benchmark.driver do |x|
  x.report 'Kernel.foo_c(42, 17)'
  x.report 'Kernel.foo_builtin(42, 17)'
  x.report 'Kernel.foo_pure_ruby(42, 17)'
end
