Pod::Spec.new do |s|
  s.name             = 'intercom_core'
  s.version          = '0.1.0'
  s.summary          = 'Core SyncN intercom plugin.'
  s.description      = 'Protocol, call engine, and native audio/video for SyncN intercom.'
  s.homepage         = 'https://syncn.local'
  s.license          = { :type => 'MIT' }
  s.author           = { 'SyncN' => 'dev@syncn.local' }
  s.source           = { :path => '.' }
  s.source_files = 'Classes/**/*'
  s.dependency 'Flutter'
  s.platform = :ios, '12.0'
  s.swift_version = '5.0'
  s.frameworks = ['VideoToolbox', 'AVFoundation', 'CoreMedia', 'AudioToolbox']
end
