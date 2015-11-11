%define ALTERED_DESTROY(class, modulec, delete_class)
%feature("shadow") ~class()
%{
sub DESTROY {
    my $self;
    if ($_[0]->isa('SCALAR')) {
        $self = $_[0];
    } else {
        return unless $_[0]->isa('HASH');
        $self = tied(%{$_[0]});
        return unless defined $self;
    }
    my $code = $Geo::GDAL::stdout_redirection{$self};
    delete $Geo::GDAL::stdout_redirection{$self};
    delete $ITERATORS{$self};
    if (exists $OWNER{$self}) {
        Geo::modulec::delete_class($self);
        delete $OWNER{$self};
    }
    $self->RELEASE_PARENTS();
    if ($code) {
        Geo::GDAL::VSIStdoutUnsetRedirection();
        $code->close;
    }
    
}
%}
%enddef
