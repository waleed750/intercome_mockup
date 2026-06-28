Pod::Spec.new do |s|
  s.name             = 'intercom'
  s.version          = '0.1.0'
  s.summary          = 'Shared SyncN intercom package.'
  s.description      = 'Shared SyncN intercom package for panel and mobile apps.'
  s.homepage         = 'https://syncn.local'
  s.license          = { :type => 'MIT' }
  s.author           = { 'SyncN' => 'dev@syncn.local' }
  s.source           = { :path => '.' }
  s.source_files = 'Classes/**/*'
  s.dependency 'Flutter'
  s.platform = :ios, '12.0'
  s.swift_version = '5.0'
end
